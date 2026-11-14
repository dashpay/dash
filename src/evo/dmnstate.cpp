// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/dmnstate.h>

#include <clientversion.h>
#include <script/standard.h>
#include <streams.h>

std::string CDeterministicMNState::ToString() const
{
    CTxDestination dest;
    std::string payoutAddress = "unknown";
    std::string operatorPayoutAddress = "none";
    if (ExtractDestination(scriptPayout, dest)) {
        payoutAddress = EncodeDestination(dest);
    }
    const std::string payoutList = IsShared() ? PayoutListToString(shares, nEarlyPeriodBlocks, nEarlyPenalty)
                                            : PayoutListToString(GetOwnerPayouts(*this));
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        operatorPayoutAddress = EncodeDestination(dest);
    }

    return strprintf("CDeterministicMNState(nVersion=%d, nRegisteredHeight=%d, nLastPaidHeight=%d, nPoSePenalty=%d, "
                     "nPoSeRevivedHeight=%d, nPoSeBanHeight=%d, nRevocationReason=%d, "
                     "ownerAddress=%s, pubKeyOperator=%s, votingAddress=%s, netInfo=%s, payoutAddress=%s, payouts=%s, "
                     "operatorPayoutAddress=%s)\n",
                     nVersion, nRegisteredHeight, nLastPaidHeight, nPoSePenalty, nPoSeRevivedHeight, nPoSeBanHeight,
                     nRevocationReason, EncodeDestination(PKHash(keyIDOwner)),
                     pubKeyOperator.ToString(OperatorKeyScheme()), EncodeDestination(PKHash(keyIDVoting)),
                     netInfo->ToString(), payoutAddress, payoutList, operatorPayoutAddress);
}

void CDeterministicMNStateDiffLegacy::MaterializeOperatorKeyFromBytes()
{
    if ((fields & LegacyField_pubKeyOperator) == 0) {
        return;
    }
    // No version in the diff: hold the raw bytes under legacy until the masternode's version
    // is known. A present version selects the scheme those bytes are decoded with.
    // NOLINTNEXTLINE(bls-legacy-scheme)
    const BLSScheme scheme{(fields & LegacyField_nVersion) ? state.OperatorKeyScheme() : BLSScheme::Legacy};
    CDataStream stream{Span<const uint8_t>{pubKeyOperatorBytes}, SER_DISK, CLIENT_VERSION};
    state.pubKeyOperator.Unserialize(stream, scheme);
}

std::vector<CScript> CDeterministicMNState::GetOwnerRewardScripts() const
{
    std::vector<CScript> ret;
    if (IsShared()) {
        ret.reserve(shares.size());
        for (const auto& share : shares) {
            ret.emplace_back(share.RewardScript());
        }
    } else {
        for (const auto& payout : GetOwnerPayouts(*this)) {
            ret.emplace_back(payout.scriptPayout);
        }
    }
    return ret;
}
