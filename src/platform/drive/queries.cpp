// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/drive/queries.h>

#include <dash/platform/ffi.h>

#include <algorithm>

namespace platform::drive {
namespace {

rust::Slice<const uint8_t> ToSlice(Span<const uint8_t> data)
{
    return {data.data(), data.size()};
}

//! Copies the authenticated metadata fields out of the bridge result.
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

bool IdFromFfi(const rust::Vec<uint8_t>& bytes, Identifier& id_out, std::string& error)
{
    if (bytes.size() != id_out.size()) {
        error = "bridge returned a malformed identifier";
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), id_out.begin());
    return true;
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

//! Runs one bridge call, converting the rust::Error it throws on a failed or
//! malformed proof into the bool + error-string convention used here.
template <typename Fn>
bool Bridged(std::string& error, const Fn& fn)
{
    try {
        return fn();
    } catch (const std::exception& e) {
        // rust::Error from the bridge, but also cxx marshalling throws such
        // as rust::String rejecting invalid UTF-8.
        error = e.what();
        return false;
    }
}

bool IdentityFromFfi(const platform_ffi::FfiVerifiedIdentity& result,
                     std::optional<Identity>& identity_out, ResponseMetadata& meta_out,
                     std::string& error)
{
    meta_out = MetaFromFfi(result.meta);
    if (!result.present) {
        identity_out = std::nullopt; // proven absent
        return true;
    }
    Identity identity;
    if (!IdFromFfi(result.identity.id, identity.id, error)) return false;
    identity.balance = result.identity.balance;
    identity.revision = result.identity.revision;
    identity.public_keys.reserve(result.identity.keys.size());
    for (const platform_ffi::FfiIdentityKey& key : result.identity.keys) {
        identity.public_keys.push_back(KeyFromFfi(key));
    }
    identity_out = std::move(identity);
    return true;
}

} // namespace

bool SetContext(const std::string& network_id, uint32_t platform_activation_height,
                std::string& error)
{
    return Bridged(error, [&] {
        platform_ffi::set_context(network_id, platform_activation_height);
        return true;
    });
}

bool UpdateQuorumKeys(uint8_t llmq_type, const std::vector<BridgeQuorumKey>& keys,
                      std::string& error)
{
    return Bridged(error, [&] {
        rust::Vec<platform_ffi::FfiQuorumKey> ffi_keys;
        ffi_keys.reserve(keys.size());
        for (const BridgeQuorumKey& key : keys) {
            platform_ffi::FfiQuorumKey ffi_key;
            ffi_key.quorum_hash.reserve(key.proof_hash.size());
            for (const uint8_t byte : key.proof_hash) ffi_key.quorum_hash.push_back(byte);
            ffi_key.pubkey.reserve(key.pubkey.size());
            for (const uint8_t byte : key.pubkey) ffi_key.pubkey.push_back(byte);
            ffi_keys.push_back(std::move(ffi_key));
        }
        platform_ffi::update_quorum_keys(llmq_type, std::move(ffi_keys));
        return true;
    });
}

bool VerifyGetIdentityNonce(Span<const uint8_t> request, Span<const uint8_t> response,
                            std::optional<uint64_t>& nonce_out, ResponseMetadata& meta_out,
                            std::string& error)
{
    return Bridged(error, [&] {
        const platform_ffi::FfiVerifiedU64 result{
            platform_ffi::verify_get_identity_nonce(ToSlice(request), ToSlice(response))};
        nonce_out = result.present ? std::optional<uint64_t>{result.value} : std::nullopt;
        meta_out = MetaFromFfi(result.meta);
        return true;
    });
}

bool VerifyGetIdentityContractNonce(Span<const uint8_t> request, Span<const uint8_t> response,
                                    std::optional<uint64_t>& nonce_out,
                                    ResponseMetadata& meta_out, std::string& error)
{
    return Bridged(error, [&] {
        const platform_ffi::FfiVerifiedU64 result{platform_ffi::verify_get_identity_contract_nonce(
            ToSlice(request), ToSlice(response))};
        nonce_out = result.present ? std::optional<uint64_t>{result.value} : std::nullopt;
        meta_out = MetaFromFfi(result.meta);
        return true;
    });
}

bool VerifyGetIdentity(Span<const uint8_t> request, Span<const uint8_t> response,
                       std::optional<Identity>& identity_out, ResponseMetadata& meta_out,
                       std::string& error)
{
    return Bridged(error, [&] {
        return IdentityFromFfi(
            platform_ffi::verify_get_identity(ToSlice(request), ToSlice(response)), identity_out,
            meta_out, error);
    });
}

bool VerifyGetIdentityByPubKeyHash(Span<const uint8_t> request, Span<const uint8_t> response,
                                   std::optional<Identity>& identity_out,
                                   ResponseMetadata& meta_out, std::string& error)
{
    return Bridged(error, [&] {
        return IdentityFromFfi(
            platform_ffi::verify_get_identity_by_pubkey_hash(ToSlice(request), ToSlice(response)),
            identity_out, meta_out, error);
    });
}

bool VerifyGetDocuments(Span<const uint8_t> request, Span<const uint8_t> response,
                        std::vector<Bytes>& documents_out, ResponseMetadata& meta_out,
                        std::string& error)
{
    return Bridged(error, [&] {
        const platform_ffi::FfiVerifiedDocs result{
            platform_ffi::verify_get_documents(ToSlice(request), ToSlice(response))};
        documents_out.clear();
        documents_out.reserve(result.documents.size());
        for (const platform_ffi::FfiBytes& doc : result.documents) {
            documents_out.emplace_back(doc.data.begin(), doc.data.end());
        }
        meta_out = MetaFromFfi(result.meta);
        return true;
    });
}

bool VerifyGetContestedVoteState(Span<const uint8_t> request, Span<const uint8_t> response,
                                 ContestedVoteState& out, ResponseMetadata& meta_out,
                                 std::string& error)
{
    return Bridged(error, [&] {
        const platform_ffi::FfiVerifiedContested result{
            platform_ffi::verify_get_contested_vote_state(ToSlice(request), ToSlice(response))};
        meta_out = MetaFromFfi(result.meta);

        out = ContestedVoteState{};
        out.contest_found = result.contest_found;
        out.contenders.reserve(result.contenders.size());
        for (const platform_ffi::FfiContender& contender : result.contenders) {
            Identifier id{};
            if (!IdFromFfi(contender.identity, id, error)) return false;
            out.contenders.emplace_back(id, contender.has_votes ? contender.votes : 0);
        }
        if (result.has_abstain) out.abstain_votes = result.abstain_votes;
        if (result.has_lock) out.lock_votes = result.lock_votes;
        out.finished = result.finished;
        out.locked = result.locked;
        if (result.has_winner) {
            Identifier winner{};
            if (!IdFromFfi(result.winner, winner, error)) return false;
            out.winner = winner;
        }
        out.finished_at_time_ms = result.finished_at_time_ms;
        return true;
    });
}

} // namespace platform::drive
