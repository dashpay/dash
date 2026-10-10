// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/client.h>

#include <platform/marshal.h>
#include <platform/st.h>
#include <util/translation.h>

#include <dash/platform/ffi.h>

#include <logging.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

namespace {

//! Runs one bridge call on the worker thread and delivers its outcome. The
//! bridge reports failures through Status; what still escapes as a C++
//! exception is a bug (a rust::Error from a marshalling failure such as
//! invalid UTF-8) and is delivered as INTERNAL.
template <typename T, typename Fn>
void Deliver(const PlatformClient::Callback<T>& cb, const Fn& fn)
{
    Result<T> out;
    try {
        out = fn();
    } catch (const std::exception& e) {
        out = Result<T>{};
        out.status.kind = StatusKind::INTERNAL;
        out.status.message = e.what();
    }
    cb(std::move(out));
}

class SdkClient final : public PlatformClient
{
public:
    SdkClient(const ClientConfig& config, rust::Box<platform_ffi::PlatformClient> sdk) :
        m_llmq_type(config.platform_llmq_type),
        m_sdk(std::move(sdk))
    {
        m_worker = std::thread([this] { Run(); });
    }
    ~SdkClient() override { shutdown(); }

    void shutdown() override
    {
        if (m_stop.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_queue.clear();
        }
        m_cv.notify_all();
        // Aborts the SDK request the worker may be blocked in, then joins.
        m_sdk->shutdown();
        if (m_worker.joinable()) m_worker.join();
    }

    void updateEndpoints(std::vector<Endpoint> endpoints) override
    {
        std::vector<rust::String> uris;
        uris.reserve(endpoints.size());
        for (const Endpoint& endpoint : endpoints) {
            uris.emplace_back("https://" + endpoint.service.ToStringAddrPort());
        }
        try {
            // Replace the SDK set even when it is empty, so endpoints removed
            // from the deterministic masternode list cannot remain usable.
            m_sdk->set_endpoints(rust::Slice<const rust::String>{uris.data(), uris.size()});
        } catch (const std::exception& e) {
            LogPrintf("Platform client: unable to update endpoints: %s\n", e.what());
        }
    }

    void updateQuorumKeys(uint8_t llmq_type, std::vector<QuorumKey> keys) override
    {
        if (llmq_type != m_llmq_type) {
            LogPrintf("Platform client: ignoring quorum keys of LLMQ type %d (Platform type is %d)\n", llmq_type,
                      m_llmq_type);
            return;
        }
        // The shell takes Core's internal uint256 order and normalizes it to
        // the order proofs carry; nothing here knows the wire representation.
        std::vector<platform_ffi::QuorumKey> ffi_keys;
        ffi_keys.reserve(keys.size());
        for (const QuorumKey& key : keys) {
            platform_ffi::QuorumKey ffi_key;
            if (key.pubkey.size() != ffi_key.pubkey.size()) continue;
            std::copy(key.quorum_hash.begin(), key.quorum_hash.end(), ffi_key.hash.begin());
            std::copy(key.pubkey.begin(), key.pubkey.end(), ffi_key.pubkey.begin());
            ffi_keys.push_back(std::move(ffi_key));
        }
        try {
            m_sdk->set_quorum_keys(rust::Slice<const platform_ffi::QuorumKey>{ffi_keys.data(), ffi_keys.size()});
        } catch (const std::exception& e) {
            LogPrintf("Platform client: unable to update quorum keys: %s\n", e.what());
        }
    }

    void updateCoreChainLockedHeight(int32_t height) override
    {
        if (height <= 0) return;
        try {
            m_sdk->set_chainlock_height(static_cast<uint32_t>(height));
        } catch (const std::exception& e) {
            LogPrintf("Platform client: unable to update the ChainLock height: %s\n", e.what());
        }
    }

    void resolveName(const std::string& normalized_label, Callback<DpnsName> cb) override
    {
        Enqueue([=, this] { Deliver(cb, [&] { return marshal::FromFfi(m_sdk->resolve_name(normalized_label)); }); });
    }
    void searchNames(const std::string& prefix, uint32_t limit, const Identifier& start_after,
                     Callback<Paged<DpnsName>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&] { return marshal::FromFfi(m_sdk->search_names(prefix, limit, start_after)); });
        });
    }
    void namesOfIdentity(const Identifier& identity, const Identifier& start_after, Callback<Paged<DpnsName>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&] { return marshal::FromFfi(m_sdk->names_of_identity(identity, start_after)); });
        });
    }
    void getIdentity(const Identifier& id, Callback<Identity> cb) override
    {
        Enqueue([=, this] { Deliver(cb, [&] { return marshal::FromFfi(m_sdk->get_identity(id)); }); });
    }
    void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<Identity> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&] { return marshal::FromFfi(m_sdk->get_identity_by_pubkey_hash(pubkey_hash)); });
        });
    }
    void getIdentityContractNonce(const Identifier& id, const Identifier& contract_id, Callback<uint64_t> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&] { return marshal::FromFfi(m_sdk->get_identity_contract_nonce(id, contract_id)); });
        });
    }
    void getProfile(const Identifier& owner_id, Callback<Profile> cb) override
    {
        Enqueue([=, this] { Deliver(cb, [&] { return marshal::FromFfi(m_sdk->get_profile(owner_id)); }); });
    }
    void getContactRequests(const Identifier& identity, bool to_me, uint64_t since_ms, const Identifier& start_after,
                            Callback<Paged<ContactRequest>> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&] {
                return marshal::FromFfi(m_sdk->get_contact_requests(identity, to_me, since_ms, start_after));
            });
        });
    }
    void getContestedNameState(const std::string& normalized_label, Callback<ContestedNameState> cb) override
    {
        Enqueue([=, this] {
            Deliver(cb, [&] { return marshal::FromFfi(m_sdk->get_contested_vote_state(normalized_label)); });
        });
    }
    void broadcastStateTransition(const std::vector<uint8_t>& state_transition, BroadcastCallback cb) override
    {
        Enqueue([=, this] {
            Status status;
            try {
                status = marshal::FromFfi(
                    m_sdk->broadcast(rust::Slice<const uint8_t>{state_transition.data(), state_transition.size()}).status);
            } catch (const std::exception& e) {
                status.kind = StatusKind::INTERNAL;
                status.message = e.what();
            }
            cb(std::move(status));
        });
    }

    util::Result<Built> buildIdentityCreate(const SigningOperation& op, const AssetLockProof& proof,
                                            const std::vector<NewIdentityKey>& keys) override
    {
        return st::BuildIdentityCreate(*m_sdk, op, proof, keys);
    }
    util::Result<Built> buildDpnsPreorder(const SigningOperation& op, const Identifier& owner, uint64_t nonce,
                                          const std::string& label, const std::array<uint8_t, 32>& salt) override
    {
        return st::BuildDpnsPreorder(*m_sdk, op, owner, nonce, label, salt);
    }
    util::Result<Built> buildDpnsDomain(const SigningOperation& op, const Identifier& owner, uint64_t nonce,
                                        const std::string& label, const std::array<uint8_t, 32>& salt) override
    {
        return st::BuildDpnsDomain(*m_sdk, op, owner, nonce, label, salt);
    }
    util::Result<Built> buildProfile(const SigningOperation& op, const Identifier& owner, uint64_t nonce,
                                     const Profile& existing, const ProfileInput& input) override
    {
        return st::BuildProfile(*m_sdk, op, owner, nonce, existing, input);
    }
    util::Result<Built> buildContactRequest(const SigningOperation& op, const Identity& sender, const Identity& recipient,
                                            uint64_t nonce, const ContactRequestInput& input) override
    {
        return st::BuildContactRequest(*m_sdk, op, sender, recipient, nonce, input);
    }
    util::Result<uint64_t> contestedVoteFundCredits() override
    {
        try {
            return m_sdk->contested_vote_fund_credits();
        } catch (const std::exception& e) {
            return util::Error{Untranslated(e.what())};
        }
    }

private:
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

    const uint8_t m_llmq_type;
    rust::Box<platform_ffi::PlatformClient> m_sdk;
    std::thread m_worker;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    std::atomic_bool m_stop{false};
};

} // namespace

std::unique_ptr<PlatformClient> MakeSdkPlatformClient(const ClientConfig& config)
{
    platform_ffi::Config ffi_config;
    ffi_config.network = static_cast<uint8_t>(config.network);
    ffi_config.platform_llmq_type = config.platform_llmq_type;
    ffi_config.proxy = marshal::ToFfi(config.proxy);
    try {
        return std::make_unique<SdkClient>(config, platform_ffi::new_platform_client(ffi_config));
    } catch (const std::exception& e) {
        LogPrintf("Platform client: unable to initialize the SDK: %s\n", e.what());
        return nullptr;
    }
}

} // namespace platform
