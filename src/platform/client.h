// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_CLIENT_H
#define BITCOIN_PLATFORM_CLIENT_H

#include <netaddress.h>
#include <platform/types.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace platform {

//! A quorum public key the client may verify proofs against, fed from the
//! node's locally synced LLMQ data (interfaces::Node::LLMQ).
struct QuorumKey {
    uint256 quorum_hash;
    std::vector<uint8_t> pubkey; //!< serialized BLS public key (basic scheme)
    int32_t height{0};

    //! DAPI encodes Proof.quorum_hash in display (big-endian) byte order,
    //! while uint256::begin() exposes Dash Core's internal little-endian
    //! representation. Keep that representation boundary explicit here so a
    //! valid locally synced quorum can be selected without weakening proof
    //! verification.
    bool matchesProofHash(const std::array<uint8_t, 32>& proof_hash) const
    {
        for (size_t i = 0; i < proof_hash.size(); ++i) {
            if (proof_hash[i] != quorum_hash.begin()[proof_hash.size() - 1 - i]) return false;
        }
        return true;
    }
};

//! An evonode DAPI endpoint, fed from the deterministic masternode list.
struct Endpoint {
    CService service;      //!< platform HTTPS (gRPC-Web gateway) addr:port
    uint256 pro_tx_hash{};

    friend bool operator==(const Endpoint& lhs, const Endpoint& rhs)
    {
        return lhs.service == rhs.service && lhs.pro_tx_hash == rhs.pro_tx_hash;
    }
};

template <typename T>
struct Result {
    std::optional<T> value;
    std::string error;
    ResponseMetadata metadata;

    bool ok() const { return value.has_value(); }
};

//! Abstract asynchronous Dash Platform (DAPI) client.
//!
//! Implementations own their I/O threads; callbacks fire on client threads
//! and consumers must marshal to their own thread (the Qt layer uses
//! QMetaObject::invokeMethod). Every query is issued with prove=true and the
//! response is verified (GroveDB proof replay to the root hash, then the
//! Tenderdash quorum signature against the quorum keys supplied via
//! updateQuorumKeys) before the callback sees it; unverifiable responses
//! surface as errors and the offending endpoint is rotated out.
class PlatformClient
{
public:
    template <typename T>
    using Callback = std::function<void(Result<T>)>;

    virtual ~PlatformClient() = default;

    //! Resolve an exact normalized label under a parent domain ("dash").
    //! Absence (name available) yields ok() with std::nullopt-like empty
    //! optional inside the vector-free overloads below; here: value with
    //! std::optional<DpnsName> empty means proven absent.
    virtual void resolveName(const std::string& normalized_label, Callback<std::optional<DpnsName>> cb) = 0;

    //! Prefix search over normalizedLabel (startsWith), limited.
    virtual void searchNames(const std::string& prefix, uint32_t limit, Callback<std::vector<DpnsName>> cb) = 0;

    //! Reverse lookup: all names whose records.identity == identity.
    virtual void namesOfIdentity(const Identifier& identity, Callback<std::vector<DpnsName>> cb) = 0;

    virtual void getIdentity(const Identifier& id, Callback<std::optional<Identity>> cb) = 0;
    virtual void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& pubkey_hash, Callback<std::optional<Identity>> cb) = 0;
    virtual void getIdentityNonce(const Identifier& id, Callback<uint64_t> cb) = 0;
    virtual void getIdentityContractNonce(const Identifier& id, const Identifier& contract_id, Callback<uint64_t> cb) = 0;

    virtual void getProfile(const Identifier& owner_id, Callback<std::optional<Profile>> cb) = 0;
    //! Contact requests sent to (to_me=true) or by (to_me=false) the given
    //! identity, created at or after since_ms (0 = all).
    virtual void getContactRequests(const Identifier& identity, bool to_me, uint64_t since_ms, Callback<std::vector<ContactRequest>> cb) = 0;

    virtual void getContestedNameState(const std::string& normalized_label, Callback<ContestedNameState> cb) = 0;

    //! Broadcast a serialized state transition. The result's error channel is
    //! unverified; confirm success with a proved re-query of the created
    //! object (waitForStateTransitionResult is used internally to surface
    //! consensus errors quickly).
    virtual void broadcastStateTransition(const std::vector<uint8_t>& state_transition, Callback<BroadcastResult> cb) = 0;

    //! Node-local trust/context injection.
    virtual void updateEndpoints(std::vector<Endpoint> endpoints) = 0;
    virtual void updateQuorumKeys(uint8_t llmq_type, std::vector<QuorumKey> keys) = 0;
    //! The node's best locally verified core ChainLock height. Used as a
    //! coarse staleness floor so an on-path attacker cannot replay a much
    //! older (but validly signed) platform state. 0 means "unknown" and
    //! disables the floor.
    virtual void updateCoreChainLockedHeight(int32_t height) = 0;

    //! Stop all I/O and drop pending callbacks (must be called before the
    //! consumer is destroyed).
    virtual void shutdown() = 0;
};

//! Create the production gRPC-Web/TLS client (implemented in
//! platform/transport/, Phase 7).
std::unique_ptr<PlatformClient> MakeGrpcWebPlatformClient(const Params& params);

} // namespace platform

#endif // BITCOIN_PLATFORM_CLIENT_H
