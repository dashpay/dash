// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_PLATFORM_CLIENT_H
#define BITCOIN_TEST_UTIL_PLATFORM_CLIENT_H

#include <platform/client.h>
#include <platform/types.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

//! A scripted platform::PlatformClient for tests of the layers above it.
//! Each call records itself and takes the next result scripted for it
//! (UNAVAILABLE when none is). As with the real client, its callback runs
//! later: deliverAll() runs the pending callbacks, a write releasing its
//! unlock scope just before its callback. shutdown() drops pending callbacks
//! and later calls.
class FakePlatformClient final : public platform::PlatformClient
{
public:
    struct Call {
        std::string method;
        std::string argument;                   //!< hex identifier or hash, or the label
        std::optional<uint32_t> identity_index; //!< writes only
    };

    std::deque<platform::Result<platform::Identity>> identities;
    std::deque<platform::Result<platform::Identity>> identities_by_pubkey_hash;
    std::deque<platform::Result<platform::Identifier>> resolved_names;
    std::deque<platform::Result<platform::Names>> names;
    std::deque<platform::Result<std::vector<platform::Contest>>> contests;
    std::deque<platform::Result<platform::VoteState>> vote_states;
    //! The answers to contactRequests, by direction.
    std::deque<platform::Result<std::vector<platform::ContactRequest>>> sent_requests;
    std::deque<platform::Result<std::vector<platform::ContactRequest>>> received_requests;
    std::deque<platform::Result<platform::Identity>> registered_identities;
    std::deque<platform::Result<uint64_t>> top_ups;
    std::deque<platform::Result<platform::Identifier>> registered_names;
    std::deque<platform::Result<platform::Identifier>> sent_contact_requests;
    std::deque<platform::Result<platform::InvitationClaim>> claim_statuses;
    std::deque<platform::Result<platform::Identity>> claimed_identities;

    std::vector<Call> calls;
    //! The asset lock proofs of registerIdentity and topUpIdentity, in call order.
    std::vector<platform::AssetLockProof> proofs;
    //! The funding key of each topUpIdentity, in call order.
    std::vector<wallet::AssetLockFundingKey> top_up_keys;
    //! The invitation of each invitationClaimStatus, in call order.
    std::vector<std::shared_ptr<const platform::Invitation>> checked_invitations;
    //! The invitation and keys of each claimInvitation, in call order.
    std::vector<std::pair<std::shared_ptr<const platform::Invitation>, std::vector<platform::IdentityPublicKey>>> claims;
    //! The identity and signing key of each registerName, in call order.
    std::vector<std::pair<platform::Identifier, uint32_t>> name_signers;
    //! The contest fund of each registerName, in call order.
    std::vector<uint64_t> name_contest_funds;
    //! The input of each sendContactRequest, in call order.
    std::vector<platform::ContactRequestInput> contact_inputs;
    bool shut_down{false};

    void getIdentity(const platform::Identifier& id, Callback<platform::Identity> cb) override;
    void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<platform::Identity> cb) override;
    void resolveName(const std::string& label, Callback<platform::Identifier> cb) override;
    void namesOfIdentity(const platform::Identifier& identity, uint32_t page_limit, uint32_t max_pages,
                         Callback<platform::Names> cb) override;
    void pendingContests(const platform::Identifier& identity, Callback<std::vector<platform::Contest>> cb) override;
    void contestedVoteState(const std::string& label, Callback<platform::VoteState> cb) override;
    void contactRequests(const platform::Identifier& identity, platform::ContactDirection direction, uint64_t since_ms,
                         Callback<std::vector<platform::ContactRequest>> cb) override;
    void registerIdentity(std::vector<platform::IdentityPublicKey> keys, platform::AssetLockProof proof,
                          platform::Signing signing, Callback<platform::Identity> cb) override;
    void topUpIdentity(const platform::Identifier& identity, platform::AssetLockProof proof,
                       wallet::AssetLockFundingKey funding, platform::Signing signing, Callback<uint64_t> cb) override;
    void registerName(const std::string& label, const platform::Identifier& identity, uint32_t key_id,
                      uint64_t contest_fund, platform::Signing signing, Callback<platform::Identifier> cb) override;
    void sendContactRequest(const platform::ContactRequestInput& input, platform::Signing signing,
                            Callback<platform::Identifier> cb) override;
    void invitationClaimStatus(std::shared_ptr<const platform::Invitation> invitation,
                               Callback<platform::InvitationClaim> cb) override;
    void claimInvitation(std::shared_ptr<const platform::Invitation> invitation,
                         std::vector<platform::IdentityPublicKey> keys, platform::Signing signing,
                         Callback<platform::Identity> cb) override;
    void shutdown() override;

    //! Run the pending callbacks, including those they add.
    void deliverAll();
    //! Run the oldest pending callback only.
    void deliverNext();
    //! Number of recorded calls of a method.
    size_t countCalls(const std::string& method) const;

private:
    template <typename T>
    void Answer(Call call, std::deque<platform::Result<T>>& scripted, Callback<T> cb,
                std::unique_ptr<platform::UnlockScope> unlock = nullptr);

    std::deque<std::function<void()>> m_pending;
};

#endif // BITCOIN_TEST_UTIL_PLATFORM_CLIENT_H
