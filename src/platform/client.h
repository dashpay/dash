// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_CLIENT_H
#define BITCOIN_PLATFORM_CLIENT_H

#include <platform/types.h>
#include <span.h>
#include <wallet/platformtypes.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace interfaces {
class LLMQ;
class Wallet;
} // namespace interfaces

namespace platform {

//! Keeps the wallet unlocked while a write signs. The consumer implements it
//! (dash-qt wraps WalletModel::UnlockContext). The client destroys it on the
//! executor just before the write's callback runs, or in shutdown().
class UnlockScope
{
public:
    virtual ~UnlockScope() = default;
};

//! The wallet a write signs with. Identity keys are
//! IdentityAuthKey{identity_index, key id}. The wallet must stay loaded
//! until the client has shut down.
struct Signing {
    // cppcheck-suppress uninitMemberVarNoCtor
    interfaces::Wallet& wallet;
    uint32_t identity_index{0};
    std::unique_ptr<UnlockScope> unlock;
};

//! Runs a task later on the consumer's thread, never inline (dash-qt queues
//! it with QMetaObject::invokeMethod). The client calls it from several
//! worker threads at once until shutdown() returns. A task owns nothing: one
//! that runs after shutdown() does nothing.
using Executor = std::function<void(std::function<void()>)>;

struct ClientConfig {
    std::optional<ProxyConfig> proxy; //!< nullopt: connect directly
    //! Evonode Platform HTTPS endpoints, fixed for the client's lifetime.
    std::vector<CService> endpoints;

    friend bool operator==(const ClientConfig&, const ClientConfig&) = default;
};

//! Asynchronous Dash Platform client. Each call runs on a worker thread and
//! its callback runs through the executor, never on a worker. Writes sign
//! through the wallet in Signing; private keys stay in the wallet.
class PlatformClient
{
public:
    template <typename T>
    using Callback = std::function<void(Result<T>)>;

    virtual ~PlatformClient() = default;

    virtual void getIdentity(const Identifier& id, Callback<Identity> cb) = 0;
    //! The identity whose key hashes to pubkey_hash (HASH160).
    virtual void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<Identity> cb) = 0;
    //! The identity a DPNS name resolves to; the SDK normalizes the label.
    virtual void resolveName(const std::string& label, Callback<Identifier> cb) = 0;
    //! The names of an identity, reading at most max_pages pages of page_limit.
    virtual void namesOfIdentity(const Identifier& identity, uint32_t page_limit, uint32_t max_pages,
                                 Callback<Names> cb) = 0;
    //! The names starting with prefix, at most limit of them (not 0). The
    //! result is never complete: the SDK does not say whether more exist.
    virtual void searchNames(const std::string& prefix, uint32_t limit, Callback<Names> cb) = 0;
    //! Best effort: the SDK examines at most 100 contests.
    virtual void pendingContests(const Identifier& identity, Callback<std::vector<Contest>> cb) = 0;
    //! The tally of the vote on a contested label.
    virtual void contestedVoteState(const std::string& label, Callback<VoteState> cb) = 0;
    //! Contact requests sent by or to identity, created strictly after
    //! since_ms (0: all).
    virtual void contactRequests(const Identifier& identity, ContactDirection direction, uint64_t since_ms,
                                 Callback<std::vector<ContactRequest>> cb) = 0;

    //! Create an identity with keys, funded by an asset lock paying
    //! RegistrationFundingKey{signing.identity_index}.
    virtual void registerIdentity(std::vector<IdentityPublicKey> keys, AssetLockProof proof, Signing signing,
                                  Callback<Identity> cb) = 0;
    //! Add the asset lock paying funding to the identity's balance; the
    //! result is the new balance.
    virtual void topUpIdentity(const Identifier& identity, AssetLockProof proof, wallet::TopupFundingKey funding,
                               Signing signing, Callback<uint64_t> cb) = 0;
    //! Register a DPNS name, signed with identity key key_id. A contested
    //! name pays at most contest_fund credits into its contest (0: the fund
    //! as priced just before it is sent). The result is the domain document id.
    virtual void registerName(const std::string& label, const Identifier& identity, uint32_t key_id,
                              uint64_t contest_fund, Signing signing, Callback<Identifier> cb) = 0;
    //! The result is the contact request document id.
    virtual void sendContactRequest(const ContactRequestInput& input, Signing signing, Callback<Identifier> cb) = 0;

    //! Drop queued calls, cancel and wait for running ones, and destroy
    //! every call not yet delivered, unlock scopes included. No callback runs
    //! afterwards, and later calls are dropped: a dropped call never calls
    //! back. Call it, or destroy the client, on the executor's thread (a
    //! result callback may), never while holding cs_main or cs_wallet, which
    //! the SDK's wallet and quorum callbacks take.
    virtual void shutdown() = 0;
};

//! The client over dash-sdk-cxx, verifying proofs against the quorums llmq
//! retains; llmq must outlive the client. Network and Platform LLMQ type come
//! from Params(). Returns nullptr when the SDK refuses the configuration, such
//! as a proxy it cannot use: it never falls back to connecting directly.
std::unique_ptr<PlatformClient> MakeSdkPlatformClient(const ClientConfig& config, interfaces::LLMQ& llmq,
                                                      Executor executor);

//! DPNS rules, as the SDK applies them.
bool IsValidUsername(const std::string& label);
//! Fails closed: true when the SDK cannot answer.
bool IsContestedUsername(const std::string& label);

//! Decrypt a contact request's encryptedPublicKey with the ECDH secret.
//! The public key is not validated: check IsFullyValid() before use.
std::optional<wallet::FriendshipXpub> DecryptContactXpub(const std::array<uint8_t, 32>& shared_secret,
                                                         Span<const uint8_t> encrypted_public_key);

} // namespace platform

#endif // BITCOIN_PLATFORM_CLIENT_H
