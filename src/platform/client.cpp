// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/client.h>

#include <dash/platform/ffi.h>

#include <logging.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

namespace {

rust::Slice<const uint8_t> ToSlice(const Identifier& id)
{
    return {id.data(), id.size()};
}

bool CopyId(const rust::Vec<uint8_t>& bytes, Identifier& out)
{
    if (bytes.size() != out.size()) return false;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return true;
}

//! The signature-authenticated metadata of a verified response; the SDK
//! applied its freshness policy and the chain-id / ChainLock checks before
//! handing it back.
ResponseMetadata MetaFromFfi(const platform_ffi::FfiMeta& meta)
{
    ResponseMetadata out;
    out.height = meta.height;
    out.core_chain_locked_height = meta.core_chain_locked_height;
    out.time_ms = meta.time_ms;
    out.protocol_version = meta.protocol_version;
    out.chain_id = std::string{meta.chain_id};
    return out;
}

IdentityPublicKey KeyFromFfi(const platform_ffi::FfiIdentityKey& key)
{
    IdentityPublicKey out;
    out.id = key.id;
    out.purpose = static_cast<IdentityPublicKey::Purpose>(key.purpose);
    out.security_level = static_cast<IdentityPublicKey::SecurityLevel>(key.security_level);
    out.type = static_cast<IdentityPublicKey::Type>(key.key_type);
    out.read_only = key.read_only;
    out.data.assign(key.data.begin(), key.data.end());
    out.disabled_at = key.has_disabled_at ? std::optional<uint64_t>{key.disabled_at} : std::nullopt;
    return out;
}

bool IdentityFromFfi(const platform_ffi::FfiIdentity& in, Identity& out)
{
    if (!CopyId(in.id, out.id)) return false;
    out.balance = in.balance;
    out.revision = in.revision;
    out.public_keys.clear();
    out.public_keys.reserve(in.keys.size());
    for (const platform_ffi::FfiIdentityKey& key : in.keys) out.public_keys.push_back(KeyFromFfi(key));
    return true;
}

bool NameFromFfi(const platform_ffi::FfiDpnsName& in, DpnsName& out)
{
    out.label = std::string{in.label};
    out.normalized_label = std::string{in.normalized_label};
    out.parent_domain = std::string{in.parent_domain};
    return CopyId(in.identity, out.identity) && CopyId(in.document_id, out.document_id);
}

bool ProfileFromFfi(const platform_ffi::FfiProfile& in, Profile& out)
{
    if (!CopyId(in.document_id, out.document_id) || !CopyId(in.owner_id, out.owner_id)) return false;
    out.display_name = std::string{in.display_name};
    out.public_message = std::string{in.public_message};
    out.avatar_url = std::string{in.avatar_url};
    out.avatar_hash.assign(in.avatar_hash.begin(), in.avatar_hash.end());
    out.avatar_fingerprint.assign(in.avatar_fingerprint.begin(), in.avatar_fingerprint.end());
    out.created_at = in.created_at;
    out.updated_at = in.updated_at;
    out.revision = in.revision;
    return true;
}

bool ContactRequestFromFfi(const platform_ffi::FfiContactRequest& in, ContactRequest& out)
{
    if (!CopyId(in.owner_id, out.owner_id) || !CopyId(in.to_user_id, out.to_user_id) ||
        !CopyId(in.document_id, out.document_id)) {
        return false;
    }
    out.encrypted_public_key.assign(in.encrypted_public_key.begin(), in.encrypted_public_key.end());
    out.sender_key_index = in.sender_key_index;
    out.recipient_key_index = in.recipient_key_index;
    out.account_reference = in.account_reference;
    out.encrypted_account_label.assign(in.encrypted_account_label.begin(), in.encrypted_account_label.end());
    out.core_height_created_at = in.core_height_created_at;
    out.created_at = in.created_at;
    return true;
}

//! Runs one SDK call on the worker thread and delivers its outcome. A
//! rust::Error (transport failure, failed verification, stale metadata,
//! contained panic) becomes the result's error string.
template <typename T, typename Fn>
void Deliver(const PlatformClient::Callback<T>& cb, const Fn& fn)
{
    Result<T> out;
    try {
        fn(out);
    } catch (const std::exception& e) {
        // rust::Error from the bridge, but also cxx marshalling throws such
        // as rust::String rejecting invalid UTF-8.
        out.value.reset();
        out.error = e.what();
    }
    cb(std::move(out));
}

class SdkClient final : public PlatformClient
{
public:
    SdkClient(Params params, uint8_t platform_llmq_type)
        : m_params(std::move(params)), m_llmq_type(platform_llmq_type), m_sdk(platform_ffi::new_platform_client())
    {
        try {
            m_sdk->set_context(m_params.network_id, m_llmq_type, m_params.tenderdash_chain_id,
                               m_params.protocol_version_floor, /*platform_activation_height=*/0);
        } catch (const std::exception& e) {
            LogPrintf("Platform client: unable to set SDK context: %s\n", e.what());
        }
        m_worker = std::thread([this] { Run(); });
    }
    ~SdkClient() override { shutdown(); }

    const platform_ffi::PlatformClient& sdk() const override { return *m_sdk; }

    void shutdown() override
    {
        if (m_stop.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_queue.clear();
        }
        m_cv.notify_all();
        // Interrupts the SDK call the worker may be blocked in, then joins.
        m_sdk->shutdown();
        if (m_worker.joinable()) m_worker.join();
    }

    void updateEndpoints(std::vector<Endpoint> endpoints) override
    {
        rust::Vec<rust::String> uris;
        uris.reserve(endpoints.size());
        for (const Endpoint& endpoint : endpoints) {
            uris.push_back("https://" + endpoint.service.ToStringAddrPort());
        }
        try {
            if (uris.empty()) {
                LogPrint(BCLog::QT, "Platform client: no evonode endpoints yet\n");
                return;
            }
            m_sdk->set_endpoints(std::move(uris));
        } catch (const std::exception& e) {
            LogPrintf("Platform client: unable to update endpoints: %s\n", e.what());
        }
    }

    void updateQuorumKeys(uint8_t llmq_type, std::vector<QuorumKey> keys) override
    {
        if (llmq_type != m_llmq_type) {
            LogPrintf("Platform client: ignoring quorum keys of LLMQ type %d (Platform type is %d)\n",
                      llmq_type, m_llmq_type);
            return;
        }
        // Hand the keys over in the byte order DAPI proofs carry the quorum
        // hash (display order, the reverse of uint256's internal order; the
        // representation boundary QuorumKey::matchesProofHash documents).
        rust::Vec<platform_ffi::FfiQuorumKey> ffi_keys;
        ffi_keys.reserve(keys.size());
        for (const QuorumKey& key : keys) {
            platform_ffi::FfiQuorumKey ffi_key;
            ffi_key.quorum_hash.reserve(32);
            for (size_t i = 0; i < 32; ++i) ffi_key.quorum_hash.push_back(key.quorum_hash.begin()[31 - i]);
            ffi_key.pubkey.reserve(key.pubkey.size());
            for (const uint8_t byte : key.pubkey) ffi_key.pubkey.push_back(byte);
            ffi_keys.push_back(std::move(ffi_key));
        }
        try {
            m_sdk->update_quorum_keys(std::move(ffi_keys));
        } catch (const std::exception& e) {
            LogPrintf("Platform client: unable to update quorum keys: %s\n", e.what());
        }
    }

    void updateCoreChainLockedHeight(int32_t height) override
    {
        if (height > 0) m_sdk->set_core_chain_locked_height(static_cast<uint32_t>(height));
    }

    // Queries: each enqueues a task on the worker thread.
    void resolveName(const std::string& normalized_label, Callback<std::optional<DpnsName>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<std::optional<DpnsName>>& out) {
                const auto res{m_sdk->resolve_name(normalized_label)};
                out.metadata = MetaFromFfi(res.meta);
                if (!res.present) {
                    out.value = std::optional<DpnsName>{}; // proven absent
                    return;
                }
                DpnsName name;
                if (!NameFromFfi(res.name, name)) throw std::runtime_error("bridge returned a malformed DPNS name");
                out.value = std::move(name);
            });
        });
    }
    void searchNames(const std::string& prefix, uint32_t limit, Callback<std::vector<DpnsName>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<std::vector<DpnsName>>& out) {
                NamesResult(m_sdk->search_names(prefix, limit), out);
            });
        });
    }
    void namesOfIdentity(const Identifier& identity, Callback<std::vector<DpnsName>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<std::vector<DpnsName>>& out) {
                NamesResult(m_sdk->names_of_identity(ToSlice(identity)), out);
            });
        });
    }
    void getIdentity(const Identifier& id, Callback<std::optional<Identity>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<std::optional<Identity>>& out) {
                IdentityResult(m_sdk->get_identity(ToSlice(id)), out);
            });
        });
    }
    void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& h, Callback<std::optional<Identity>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<std::optional<Identity>>& out) {
                IdentityResult(m_sdk->get_identity_by_pubkey_hash(rust::Slice<const uint8_t>{h.data(), h.size()}), out);
            });
        });
    }
    void getIdentityNonce(const Identifier& id, Callback<uint64_t> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<uint64_t>& out) { NonceResult(m_sdk->get_identity_nonce(ToSlice(id)), out); });
        });
    }
    void getIdentityContractNonce(const Identifier& id, const Identifier& contract, Callback<uint64_t> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<uint64_t>& out) {
                NonceResult(m_sdk->get_identity_contract_nonce(ToSlice(id), ToSlice(contract)), out);
            });
        });
    }
    void getProfile(const Identifier& owner_id, Callback<std::optional<Profile>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<std::optional<Profile>>& out) {
                const auto res{m_sdk->get_profile(ToSlice(owner_id))};
                out.metadata = MetaFromFfi(res.meta);
                if (!res.present) {
                    out.value = std::optional<Profile>{}; // proven absent
                    return;
                }
                Profile profile;
                if (!ProfileFromFfi(res.profile, profile)) throw std::runtime_error("bridge returned a malformed profile");
                out.value = std::move(profile);
            });
        });
    }
    void getContactRequests(const Identifier& identity, bool to_me, uint64_t /*since_ms*/,
                            Callback<std::vector<ContactRequest>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<std::vector<ContactRequest>>& out) {
                const auto res{m_sdk->get_contact_requests(ToSlice(identity), to_me)};
                out.metadata = MetaFromFfi(res.meta);
                std::vector<ContactRequest> requests;
                requests.reserve(res.requests.size());
                for (const platform_ffi::FfiContactRequest& ffi : res.requests) {
                    ContactRequest request;
                    if (!ContactRequestFromFfi(ffi, request)) throw std::runtime_error("bridge returned a malformed contact request");
                    requests.push_back(std::move(request));
                }
                out.value = std::move(requests);
            });
        });
    }
    void getContestedNameState(const std::string& normalized_label, Callback<ContestedNameState> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<ContestedNameState>& out) {
                const auto res{m_sdk->get_contested_vote_state(normalized_label)};
                out.metadata = MetaFromFfi(res.meta);
                ContestedNameState state;
                state.normalized_label = normalized_label;
                for (const platform_ffi::FfiContender& contender : res.contenders) {
                    Identifier id;
                    if (!CopyId(contender.identity, id)) throw std::runtime_error("bridge returned a malformed contender");
                    state.contenders.emplace_back(id, contender.has_votes ? contender.votes : 0);
                }
                state.abstain_votes = res.has_abstain ? res.abstain_votes : 0;
                state.lock_votes = res.has_lock ? res.lock_votes : 0;
                if (!res.contest_found) {
                    state.status = ContestedNameState::Status::UNKNOWN;
                } else if (!res.finished) {
                    state.status = ContestedNameState::Status::CONTEST_IN_PROGRESS;
                } else if (res.locked) {
                    state.status = ContestedNameState::Status::LOCKED;
                    state.ends_at = res.finished_at_time_ms;
                } else {
                    state.status = ContestedNameState::Status::WON;
                    if (res.has_winner) {
                        Identifier winner;
                        if (!CopyId(res.winner, winner)) throw std::runtime_error("bridge returned a malformed winner");
                        state.winner = winner;
                    }
                    state.ends_at = res.finished_at_time_ms;
                }
                out.value = std::move(state);
            });
        });
    }
    void broadcastStateTransition(const std::vector<uint8_t>& st, Callback<BroadcastResult> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&](Result<BroadcastResult>& out) {
                const auto res{m_sdk->broadcast_state_transition(rust::Slice<const uint8_t>{st.data(), st.size()})};
                BroadcastResult br;
                br.accepted = res.accepted;
                br.error = std::string{res.error};
                br.error_code = res.error_code;
                out.value = std::move(br);
            });
        });
    }

private:
    void NamesResult(const platform_ffi::FfiVerifiedDpnsNames& res, Result<std::vector<DpnsName>>& out)
    {
        out.metadata = MetaFromFfi(res.meta);
        std::vector<DpnsName> names;
        names.reserve(res.names.size());
        for (const platform_ffi::FfiDpnsName& ffi : res.names) {
            DpnsName name;
            if (!NameFromFfi(ffi, name)) throw std::runtime_error("bridge returned a malformed DPNS name");
            names.push_back(std::move(name));
        }
        out.value = std::move(names);
    }
    void IdentityResult(const platform_ffi::FfiVerifiedIdentity& res, Result<std::optional<Identity>>& out)
    {
        out.metadata = MetaFromFfi(res.meta);
        if (!res.present) {
            out.value = std::optional<Identity>{}; // proven absent
            return;
        }
        Identity identity;
        if (!IdentityFromFfi(res.identity, identity)) throw std::runtime_error("bridge returned a malformed identity");
        out.value = std::move(identity);
    }
    void NonceResult(const platform_ffi::FfiVerifiedU64& res, Result<uint64_t>& out)
    {
        out.metadata = MetaFromFfi(res.meta);
        out.value = res.present ? res.value : 0; // proven absent = never used
    }

    // ---- worker plumbing ----
    void Enqueue(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_stop) return;
            m_queue.push_back(std::move(task));
        }
        m_cv.notify_one();
    }
    void Run()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this] { return m_stop || !m_queue.empty(); });
                if (m_stop) return;
                task = std::move(m_queue.front());
                m_queue.pop_front();
            }
            task();
        }
    }

    Params m_params;
    uint8_t m_llmq_type;
    rust::Box<platform_ffi::PlatformClient> m_sdk;
    std::thread m_worker;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    std::atomic_bool m_stop{false};
};

} // namespace

std::unique_ptr<PlatformClient> MakeSdkPlatformClient(const Params& params, uint8_t platform_llmq_type)
{
    return std::make_unique<SdkClient>(params, platform_llmq_type);
}

} // namespace platform
