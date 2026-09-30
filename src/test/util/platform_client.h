// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_PLATFORM_CLIENT_H
#define BITCOIN_TEST_UTIL_PLATFORM_CLIENT_H

#include <platform/client.h>
#include <platform/signer.h>
#include <platform/types.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

//! A scripted platform::PlatformClient: every call pops the next result
//! scripted for it (UNAVAILABLE when none is), records what was asked, and
//! delivers the callback synchronously. The seam every Core test drives;
//! proof-level behaviour is tested upstream against the SDK shell.
class FakePlatformClient final : public platform::PlatformClient
{
public:
    struct Call {
        std::string method;
        std::string argument; //!< hex identifier, label or prefix
        platform::Identifier start_after{};
    };

    std::deque<platform::Result<platform::DpnsName>> resolve_name;
    std::deque<platform::Result<platform::Paged<platform::DpnsName>>> search_names;
    std::deque<platform::Result<platform::Paged<platform::DpnsName>>> names_of_identity;
    std::deque<platform::Result<platform::Identity>> identities;
    std::deque<platform::Result<platform::Identity>> identities_by_pubkey_hash;
    std::deque<platform::Result<uint64_t>> nonces;
    std::deque<platform::Result<platform::Profile>> profiles;
    std::deque<platform::Result<platform::Paged<platform::ContactRequest>>> contact_requests;
    std::deque<platform::Result<platform::ContestedNameState>> contested_states;
    std::deque<platform::Status> broadcasts;
    std::deque<util::Result<platform::Built>> builds;
    std::optional<uint64_t> contested_fund_credits;

    std::vector<Call> calls;
    std::vector<platform::OperationKind> build_kinds;
    std::vector<std::vector<uint8_t>> broadcast_bytes;
    std::vector<std::vector<platform::Endpoint>> endpoint_updates;
    std::vector<std::vector<platform::QuorumKey>> quorum_key_updates;
    std::vector<int32_t> chainlock_heights;
    bool shut_down{false};

    void resolveName(const std::string& normalized_label, Callback<platform::DpnsName> cb) override;
    void searchNames(const std::string& prefix, uint32_t limit, const platform::Identifier& start_after,
                     Callback<platform::Paged<platform::DpnsName>> cb) override;
    void namesOfIdentity(const platform::Identifier& identity, const platform::Identifier& start_after,
                         Callback<platform::Paged<platform::DpnsName>> cb) override;
    void getIdentity(const platform::Identifier& id, Callback<platform::Identity> cb) override;
    void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<platform::Identity> cb) override;
    void getIdentityContractNonce(const platform::Identifier& id, const platform::Identifier& contract_id,
                                  Callback<uint64_t> cb) override;
    void getProfile(const platform::Identifier& owner_id, Callback<platform::Profile> cb) override;
    void getContactRequests(const platform::Identifier& identity, bool to_me, uint64_t since_ms,
                            const platform::Identifier& start_after,
                            Callback<platform::Paged<platform::ContactRequest>> cb) override;
    void getContestedNameState(const std::string& normalized_label, Callback<platform::ContestedNameState> cb) override;
    void broadcastStateTransition(const std::vector<uint8_t>& state_transition, BroadcastCallback cb) override;

    util::Result<platform::Built> buildIdentityCreate(const platform::SigningOperation& op,
                                                      const platform::AssetLockProof& proof,
                                                      const std::vector<platform::NewIdentityKey>& keys) override;
    util::Result<platform::Built> buildDpnsPreorder(const platform::SigningOperation& op,
                                                    const platform::Identifier& owner, uint64_t nonce,
                                                    const std::string& label, const std::array<uint8_t, 32>& salt) override;
    util::Result<platform::Built> buildDpnsDomain(const platform::SigningOperation& op,
                                                  const platform::Identifier& owner, uint64_t nonce,
                                                  const std::string& label, const std::array<uint8_t, 32>& salt) override;
    util::Result<platform::Built> buildProfile(const platform::SigningOperation& op, const platform::Identifier& owner,
                                               uint64_t nonce, const platform::Profile& existing,
                                               const platform::ProfileInput& input) override;
    util::Result<platform::Built> buildContactRequest(const platform::SigningOperation& op,
                                                      const platform::Identity& sender,
                                                      const platform::Identity& recipient, uint64_t nonce,
                                                      const platform::ContactRequestInput& input) override;
    util::Result<uint64_t> contestedVoteFundCredits() override;

    void updateEndpoints(std::vector<platform::Endpoint> endpoints) override;
    void updateQuorumKeys(uint8_t llmq_type, std::vector<platform::QuorumKey> keys) override;
    void updateCoreChainLockedHeight(int32_t height) override;
    void shutdown() override;

    //! Number of recorded calls of a method.
    size_t countCalls(const std::string& method) const;

private:
    template <typename T>
    static platform::Result<T> Pop(std::deque<platform::Result<T>>& scripted);
    util::Result<platform::Built> PopBuild(const platform::SigningOperation& op);
};

//! Scripted outcomes for FakePlatformClient.
namespace platform_test {
template <typename T>
platform::Result<T> Ok(T value)
{
    platform::Result<T> out;
    out.status.kind = platform::StatusKind::OK;
    out.metadata.height = 1;
    out.metadata.chain_id = "test";
    out.value = std::move(value);
    return out;
}
template <typename T>
platform::Result<T> Absent()
{
    platform::Result<T> out;
    out.status.kind = platform::StatusKind::PROVEN_ABSENT;
    out.metadata.height = 1;
    out.metadata.chain_id = "test";
    return out;
}
template <typename T>
platform::Result<T> Failed(platform::StatusKind kind, std::string message = "scripted failure")
{
    platform::Result<T> out;
    out.status.kind = kind;
    out.status.message = std::move(message);
    return out;
}
platform::Status BroadcastStatus(platform::StatusKind kind, uint32_t consensus_code = 0);
platform::Identifier IdentifierFromByte(uint8_t byte);
} // namespace platform_test

#endif // BITCOIN_TEST_UTIL_PLATFORM_CLIENT_H
