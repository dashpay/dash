// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_CLIENT_H
#define BITCOIN_PLATFORM_CLIENT_H

#include <netaddress.h>
#include <platform/signer.h>
#include <platform/types.h>
#include <uint256.h>
#include <util/result.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platform {

//! A quorum public key the client may verify proofs against, fed from the
//! node's locally synced LLMQ data (interfaces::Node::LLMQ). The hash is in
//! Core's internal uint256 byte order; the SDK shell normalizes it.
struct QuorumKey {
    uint256 quorum_hash;
    std::vector<uint8_t> pubkey; //!< serialized BLS public key (basic scheme)
};

//! An evonode DAPI endpoint, fed from the deterministic masternode list.
struct Endpoint {
    CService service; //!< platform HTTPS (gRPC gateway) addr:port
};

//! Which network a client serves (the SDK's view of Core's chain) and how
//! it connects, which is fixed for the client's lifetime.
struct ClientConfig {
    enum class Network : uint8_t {
        MAIN = 0,
        TESTNET = 1,
        DEVNET = 2,
        REGTEST = 3
    };
    Network network{Network::REGTEST};
    uint8_t platform_llmq_type{0};
    //! nullopt: connect to evonodes directly.
    std::optional<ProxyConfig> proxy;
};

//! Abstract asynchronous Dash Platform (DAPI) client.
//!
//! Implementations own their I/O thread; callbacks fire on that thread and
//! consumers marshal to their own (the Qt layer uses QMetaObject::invokeMethod).
//! Every read is proved: the SDK replays the GroveDB proof and verifies the
//! Tenderdash quorum signature against the keys pushed through
//! updateQuorumKeys before a callback sees the result, and absence is a
//! proven outcome (Result::provenAbsent()), never inferred from a failure.
//!
//! One call is one SDK request: paged reads return a single page and the
//! caller continues from Paged::next_start_after on a later call.
class PlatformClient
{
public:
    template <typename T>
    using Callback = std::function<void(Result<T>)>;
    using BroadcastCallback = std::function<void(Status)>;

    virtual ~PlatformClient() = default;

    //! Resolve an exact normalized label under the "dash" parent domain.
    virtual void resolveName(const std::string& normalized_label, Callback<DpnsName> cb) = 0;

    //! One page of names starting with prefix (normalizedLabel startsWith),
    //! ascending, at most limit (clamped to 1..100).
    virtual void searchNames(const std::string& prefix, uint32_t limit, const Identifier& start_after,
                             Callback<Paged<DpnsName>> cb) = 0;

    //! One page of the names whose records.identity == identity.
    virtual void namesOfIdentity(const Identifier& identity, const Identifier& start_after,
                                 Callback<Paged<DpnsName>> cb) = 0;

    virtual void getIdentity(const Identifier& id, Callback<Identity> cb) = 0;
    virtual void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<Identity> cb) = 0;

    //! The identity's nonce for a contract. PROVEN_ABSENT means the identity
    //! has not used the contract yet: the next nonce is 1, as after 0.
    virtual void getIdentityContractNonce(const Identifier& id, const Identifier& contract_id, Callback<uint64_t> cb) = 0;

    virtual void getProfile(const Identifier& owner_id, Callback<Profile> cb) = 0;

    //! One page of the contact requests sent to (to_me) or by identity,
    //! created at or after since_ms (0 = all), oldest first.
    virtual void getContactRequests(const Identifier& identity, bool to_me, uint64_t since_ms,
                                    const Identifier& start_after, Callback<Paged<ContactRequest>> cb) = 0;

    virtual void getContestedNameState(const std::string& normalized_label, Callback<ContestedNameState> cb) = 0;

    //! Broadcast a serialized state transition. The reply is advisory and
    //! typed (OK or ALREADY_EXISTS is success, CONSENSUS carries the code);
    //! every write is confirmed by a proved re-query of the created object.
    virtual void broadcastStateTransition(const std::vector<uint8_t>& state_transition, BroadcastCallback cb) = 0;

    //! State-transition builders: no network, run to completion on the
    //! calling thread, signed through the WalletSigner the operation binds.
    //! The SDK builds under the protocol version a verified read has shown
    //! the network to run and fails before the first such read.
    virtual util::Result<Built> buildIdentityCreate(const SigningOperation& op, const AssetLockProof& proof,
                                                    const std::vector<NewIdentityKey>& keys) = 0;
    virtual util::Result<Built> buildDpnsPreorder(const SigningOperation& op, const Identifier& owner, uint64_t nonce,
                                                  const std::string& label, const std::array<uint8_t, 32>& salt) = 0;
    virtual util::Result<Built> buildDpnsDomain(const SigningOperation& op, const Identifier& owner, uint64_t nonce,
                                                const std::string& label, const std::array<uint8_t, 32>& salt) = 0;
    virtual util::Result<Built> buildProfile(const SigningOperation& op, const Identifier& owner, uint64_t nonce,
                                             const Profile& existing, const ProfileInput& input) = 0;
    virtual util::Result<Built> buildContactRequest(const SigningOperation& op, const Identity& sender,
                                                    const Identity& recipient, uint64_t nonce,
                                                    const ContactRequestInput& input) = 0;

    //! Credits a contested name registration must prefund, under the
    //! protocol version a verified read has shown the network to run; an
    //! error before that.
    virtual util::Result<uint64_t> contestedVoteFundCredits() = 0;

    //! Node-local trust inputs, pushed by the consumer. An empty endpoint
    //! set is allowed and removes every endpoint. An endpoint must be an IP
    //! address, or an onion address when the client has a proxy.
    virtual void updateEndpoints(std::vector<Endpoint> endpoints) = 0;
    //! Replace the Platform quorum keys; keys of another LLMQ type are ignored.
    virtual void updateQuorumKeys(uint8_t llmq_type, std::vector<QuorumKey> keys) = 0;
    //! The node's best ChainLock height, the anchor of the proof staleness
    //! floor. Heights <= 0 are ignored.
    virtual void updateCoreChainLockedHeight(int32_t height) = 0;

    //! Stop all I/O and drop pending callbacks (must be called before the
    //! consumer is destroyed).
    virtual void shutdown() = 0;
};

//! Create the production client over the Dash Platform SDK (dash-platform-cxx),
//! fed endpoints, quorum keys and ChainLock heights from this node. Returns
//! nullptr when the SDK refuses the configuration (a proxy it cannot use
//! included: it never falls back to connecting directly).
std::unique_ptr<PlatformClient> MakeSdkPlatformClient(const ClientConfig& config);

} // namespace platform

#endif // BITCOIN_PLATFORM_CLIENT_H
