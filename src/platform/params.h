// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_PARAMS_H
#define BITCOIN_PLATFORM_PARAMS_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace platform {

using Identifier = std::array<uint8_t, 32>;

//! Well-known system data contract identifiers. These are protocol constants
//! created at Platform genesis and identical on every network.
//! Source: dashpay/platform packages/dpns-contract/src/lib.rs and
//! packages/dashpay-contract/src/lib.rs (ID_BYTES).
inline constexpr Identifier DPNS_CONTRACT_ID{
    230, 104, 198, 89, 175, 102, 174, 225, 231, 44, 24, 109, 222, 123, 91, 126,
    10, 29, 113, 42, 9, 196, 13, 87, 33, 246, 34, 191, 83, 197, 49, 85};

inline constexpr Identifier DASHPAY_CONTRACT_ID{
    162, 161, 180, 172, 111, 239, 34, 234, 42, 26, 104, 232, 18, 54, 68, 179,
    87, 135, 95, 107, 65, 44, 24, 16, 146, 129, 193, 70, 231, 178, 113, 188};

//! Document id of the pre-registered "dash" top level domain DPNS document.
//! Source: packages/dpns-contract/src/lib.rs DPNS_DASH_TLD_DOCUMENT_ID.
inline constexpr Identifier DPNS_DASH_TLD_DOCUMENT_ID{
    215, 242, 197, 63, 70, 169, 23, 171, 110, 91, 57, 162, 215, 188, 38, 11,
    100, 146, 137, 69, 55, 68, 209, 224, 212, 242, 106, 141, 142, 255, 55, 207};

//! Preorder salt of the "dash" TLD document.
//! Source: packages/dpns-contract/src/lib.rs DPNS_DASH_TLD_PREORDER_SALT.
inline constexpr Identifier DPNS_DASH_TLD_PREORDER_SALT{
    224, 181, 8, 197, 163, 104, 37, 162, 6, 105, 58, 31, 65, 74, 161, 62,
    219, 236, 244, 60, 65, 227, 199, 153, 234, 158, 115, 123, 79, 154, 162, 38};

//! Per-network Platform parameters for networks where Platform is deployed.
struct Params {
    //! Chain name as in CBaseChainParams ("main", "test", ...).
    std::string network_id;
    //! Tenderdash chain id, part of the quorum signature preimage
    //! (CanonicalVote.chain_id). The LLMQ type used by Platform to sign
    //! state roots is not duplicated here; read it from
    //! Consensus::Params::llmqTypePlatform.
    std::string tenderdash_chain_id;
    //! Default amount (in duffs) locked to fund a new identity when
    //! registering a username. Matches the DashPay mobile wallet defaults.
    int64_t default_identity_funding_amount{0};
    //! Funding used for contested names. Includes the 0.2 DASH protocol vote
    //! reserve plus headroom for identity/domain transition fees.
    int64_t contested_identity_funding_amount{0};

    //! Minimum credit conversion: 1 duff == 1000 platform credits.
    static constexpr int64_t CREDITS_PER_DUFF{1000};
};

//! Returns the Platform parameters for the given chain name
//! (CBaseChainParams::MAIN etc.), or std::nullopt if Platform is not
//! deployed on that network (in which case the GUI feature is disabled).
std::optional<Params> GetParams(const std::string& network_id);

} // namespace platform

#endif // BITCOIN_PLATFORM_PARAMS_H
