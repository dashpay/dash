// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/dmnstate.h>

#include <script/standard.h>

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
                     nRevocationReason, EncodeDestination(PKHash(keyIDOwner)), pubKeyOperator.ToString(),
                     EncodeDestination(PKHash(keyIDVoting)), netInfo->ToString(), payoutAddress, payoutList, operatorPayoutAddress);
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
