// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_DRIVE_QUERIES_H
#define BITCOIN_PLATFORM_DRIVE_QUERIES_H

#include <platform/types.h>
#include <span.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

//! Proved-response verification for the DAPI queries the dash-qt Platform
//! GUI makes. Each adapter hands the exact protobuf request the transport
//! sent plus the full protobuf response it received to the Platform-owned CXX
//! bindings, where drive-proof-verifier's
//! FromProof reconstructs the query from the request, replays the GroveDB
//! proof, and verifies the Tenderdash BLS quorum threshold signature against
//! the quorum keys previously pushed via UpdateQuorumKeys. On success,
//! `meta_out` carries the signature-authenticated ResponseMetadata fields
//! (height, core-chain-locked height, time, chain id) the caller feeds to
//! the freshness tracker.
namespace platform::drive {

using platform::Identifier;
using platform::Identity;
using platform::ResponseMetadata;

//! Pushes the network context into the bridge's verification provider:
//! the chain name ("main", "test", "regtest", "devnet") selects the dpp
//! network, and `platform_activation_height` is the core height Platform
//! activated at (0 = unknown; only consulted by query paths the GUI does
//! not use). Must be called before any Verify* adapter.
bool SetContext(const std::string& network_id, uint32_t platform_activation_height,
                std::string& error);

//! A quorum BLS public key in the byte order DAPI proofs carry the quorum
//! hash (display order — the reverse of Dash Core's internal uint256 byte
//! order).
struct BridgeQuorumKey {
    std::array<uint8_t, 32> proof_hash{};
    std::vector<uint8_t> pubkey; //!< 48-byte basic-scheme BLS public key
};

//! Replaces the bridge-side key set of `llmq_type` with `keys` (the client
//! pushes the full active Platform-LLMQ set on every update).
bool UpdateQuorumKeys(uint8_t llmq_type, const std::vector<BridgeQuorumKey>& keys,
                      std::string& error);

//! getIdentityNonce / getIdentityContractNonce. `nonce_out` is std::nullopt
//! when the nonce is cryptographically proven absent.
bool VerifyGetIdentityNonce(Span<const uint8_t> request, Span<const uint8_t> response,
                            std::optional<uint64_t>& nonce_out, ResponseMetadata& meta_out,
                            std::string& error);
bool VerifyGetIdentityContractNonce(Span<const uint8_t> request, Span<const uint8_t> response,
                                    std::optional<uint64_t>& nonce_out,
                                    ResponseMetadata& meta_out, std::string& error);

//! getIdentity: one proof covering balance, revision and keys
//! (Drive::verify_full_identity_by_identity_id). `identity_out` is
//! std::nullopt when the identity is proven absent.
bool VerifyGetIdentity(Span<const uint8_t> request, Span<const uint8_t> response,
                       std::optional<Identity>& identity_out, ResponseMetadata& meta_out,
                       std::string& error);

//! getIdentityByPublicKeyHash: one proof resolving the unique key hash to
//! the full identity. `identity_out` is std::nullopt when no identity has
//! registered that key hash.
bool VerifyGetIdentityByPubKeyHash(Span<const uint8_t> request, Span<const uint8_t> response,
                                   std::optional<Identity>& identity_out,
                                   ResponseMetadata& meta_out, std::string& error);

//! getDocuments (DPNS domain / DashPay profile & contactRequest). The Drive
//! query — including cryptographic absence — is reconstructed from the
//! request's contract id, document type, where/order-by clauses and limit;
//! the verified documents come back platform-serialized (the input format
//! of the dpp::Decode* helpers). An empty vector means proven "no matches".
bool VerifyGetDocuments(Span<const uint8_t> request, Span<const uint8_t> response,
                        std::vector<Bytes>& documents_out, ResponseMetadata& meta_out,
                        std::string& error);

//! Decoded contested-resource vote state
//! (getContestedResourceVoteState, result type VoteTally with locked and
//! abstaining tallies).
struct ContestedVoteState {
    //! False when the contest is proven absent (no active or finished
    //! contest for the resource).
    bool contest_found{false};
    std::vector<std::pair<Identifier, uint32_t>> contenders; //!< identity -> votes
    std::optional<uint32_t> abstain_votes;
    std::optional<uint32_t> lock_votes;
    //! True once the poll's stored info reports awarded or locked; the
    //! tallies and contenders then come from the final vote event.
    bool finished{false};
    bool locked{false};               //!< finished with the name locked
    std::optional<Identifier> winner; //!< finished and awarded to this identity
    uint64_t finished_at_time_ms{0};
};

//! getContestedResourceVoteState: the poll (contract, document type, index
//! values, tally options, count) is reconstructed from the request.
bool VerifyGetContestedVoteState(Span<const uint8_t> request, Span<const uint8_t> response,
                                 ContestedVoteState& out, ResponseMetadata& meta_out,
                                 std::string& error);

} // namespace platform::drive

#endif // BITCOIN_PLATFORM_DRIVE_QUERIES_H
