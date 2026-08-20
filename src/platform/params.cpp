// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/params.h>

#include <chainparamsbase.h>

namespace platform {

std::optional<Params> GetParams(const std::string& network_id)
{
    // Tenderdash chain ids from dashpay/platform
    // packages/dashmate/configs/defaults/get{Mainnet,Testnet}ConfigFactory.js.
    // The testnet chain id changes when testnet Platform is reset; it is kept
    // overridable through the GUI-only -platformchainid argument (see
    // qt/platform/).
    if (network_id == CBaseChainParams::MAIN) {
        return Params{
            .network_id = network_id,
            .tenderdash_chain_id = "evo1",
            // 0.01 DASH; matches the DashPay mobile wallet default for an
            // uncontested username registration. Contested (premium) names
            // require additional prefunded balance, handled by the GUI flow.
            .default_identity_funding_amount = 1000000,
            .contested_identity_funding_amount = 25000000,
        };
    }
    if (network_id == CBaseChainParams::TESTNET) {
        return Params{
            .network_id = network_id,
            .tenderdash_chain_id = "dash-testnet-51",
            .default_identity_funding_amount = 1000000,
            .contested_identity_funding_amount = 25000000,
        };
    }
    // Platform is not deployed on this network (or, for devnets, the GUI
    // requires explicit -platformchainid configuration).
    return std::nullopt;
}

} // namespace platform
