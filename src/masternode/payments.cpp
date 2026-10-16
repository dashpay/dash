// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/payments.h>

#include <evo/deterministicmns.h>

#include <chain.h>
#include <consensus/amount.h>
#include <consensus/params.h>
#include <key_io.h>
#include <logging.h>
#include <script/standard.h>
#include <util/std23.h>

#include <algorithm>
#include <cassert>
#include <ranges>

int FindUnmatchedMasternodePayment(const std::vector<CTxOut>& expected,
                                   const std::vector<CTxOut>& actual,
                                   bool strict_multiplicity)
{
    if (!strict_multiplicity) {
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto& txout = expected[i];
            if (!std::ranges::any_of(actual, [&txout](const auto& txout2) { return txout == txout2; })) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    std::vector<bool> consumed(actual.size(), false);
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto& txout = expected[i];
        bool found = false;
        for (size_t j = 0; j < actual.size(); ++j) {
            if (!consumed[j] && actual[j] == txout) {
                consumed[j] = true;
                found = true;
                break;
            }
        }
        if (!found) return static_cast<int>(i);
    }
    return -1;
}

bool HasDistinctRequiredPayments(const std::vector<CTxOut>& masternode_payments,
                                 const std::vector<CTxOut>& superblock_payments, const std::vector<CTxOut>& actual)
{
    std::vector<CTxOut> required{masternode_payments};
    required.insert(required.end(), superblock_payments.begin(), superblock_payments.end());
    return FindUnmatchedMasternodePayment(required, actual, /*strict_multiplicity=*/true) < 0;
}

CAmount PlatformShare(const CAmount reward)
{
    const CAmount platformReward = reward * 375 / 1000;
    bool ok = MoneyRange(platformReward);
    assert(ok);
    return platformReward;
}

CAmount GetMasternodePayment(int nHeight, CAmount blockValue, const Consensus::Params& consensus_params, MnRewardEra era)
{
    CAmount ret = blockValue/5; // start at 20%

    const int nMNPIBlock = consensus_params.nMasternodePaymentsIncreaseBlock;
    const int nMNPIPeriod = consensus_params.nMasternodePaymentsIncreasePeriod;
    const int nReallocActivationHeight = consensus_params.BRRHeight;

                                                                      // mainnet:
    if(nHeight > nMNPIBlock)                  ret += blockValue / 20; // 158000 - 25.0% - 2014-10-24
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 1)) ret += blockValue / 20; // 175280 - 30.0% - 2014-11-25
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 2)) ret += blockValue / 20; // 192560 - 35.0% - 2014-12-26
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 3)) ret += blockValue / 40; // 209840 - 37.5% - 2015-01-26
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 4)) ret += blockValue / 40; // 227120 - 40.0% - 2015-02-27
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 5)) ret += blockValue / 40; // 244400 - 42.5% - 2015-03-30
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 6)) ret += blockValue / 40; // 261680 - 45.0% - 2015-05-01
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 7)) ret += blockValue / 40; // 278960 - 47.5% - 2015-06-01
    if(nHeight > nMNPIBlock+(nMNPIPeriod* 9)) ret += blockValue / 40; // 313520 - 50.0% - 2015-08-03

    if (nHeight < nReallocActivationHeight) {
        // Block Reward Realocation is not activated yet, nothing to do
        return ret;
    }

    int nSuperblockCycle = consensus_params.nSuperblockCycle;
    // Actual realocation starts in the cycle next to one activation happens in
    int nReallocStart = nReallocActivationHeight - nReallocActivationHeight % nSuperblockCycle + nSuperblockCycle;

    if (nHeight < nReallocStart) {
        // Activated but we have to wait for the next cycle to start realocation, nothing to do
        return ret;
    }

    if (era != MnRewardEra::Classic) {
        // Once MNRewardReallocated activates, block reward is 80% of block subsidy (+ tx fees) since treasury is 20%
        // Since the MN reward needs to be equal to 60% of the block subsidy (according to the proposal), MN reward is set to 75% of the block reward.
        // Previous reallocation periods are dropped.
        return blockValue * 3 / 4;
    }

    // Periods used to reallocate the masternode reward from 50% to 60%
    static std::vector<int> vecPeriods{
        513, // Period 1:  51.3%
        526, // Period 2:  52.6%
        533, // Period 3:  53.3%
        540, // Period 4:  54%
        546, // Period 5:  54.6%
        552, // Period 6:  55.2%
        557, // Period 7:  55.7%
        562, // Period 8:  56.2%
        567, // Period 9:  56.7%
        572, // Period 10: 57.2%
        577, // Period 11: 57.7%
        582, // Period 12: 58.2%
        585, // Period 13: 58.5%
        588, // Period 14: 58.8%
        591, // Period 15: 59.1%
        594, // Period 16: 59.4%
        597, // Period 17: 59.7%
        599, // Period 18: 59.9%
        600  // Period 19: 60%
    };

    int nReallocCycle = nSuperblockCycle * 3;
    int nCurrentPeriod = std::min<int>((nHeight - nReallocStart) / nReallocCycle, vecPeriods.size() - 1);

    return static_cast<CAmount>(blockValue * vecPeriods[nCurrentPeriod] / 1000);
}

bool IsSuperblockHeight(const int nBlockHeight, const Consensus::Params& consensus_params)
{
    // SUPERBLOCKS CAN HAPPEN ONLY after hardfork and only ONCE PER CYCLE
    return nBlockHeight >= consensus_params.nSuperblockStartBlock &&
           ((nBlockHeight % consensus_params.nSuperblockCycle) == 0);
}

bool IsSuperblockValid(const SuperblockStatus& superblock, const CTransaction& txNew, CAmount blockReward, bool is_v24, CAmount nPaymentsLimit)
{
    const std::vector<CTxOut>& payments{superblock.payments};

    // CONFIGURE SUPERBLOCK OUTPUTS

    int nOutputs = txNew.vout.size();
    int nPayments = payments.size();
    int nMinerAndMasternodePayments = nOutputs - nPayments;

    LogPrint(BCLog::GOBJECT, "%s -- nOutputs = %d, nPayments = %d\n", __func__, nOutputs, nPayments);

    // We require an exact match (including order) between the expected
    // superblock payments and the payments actually in the block.

    if (nMinerAndMasternodePayments < 0) {
        // This means the block cannot have all the superblock payments
        // so it is not valid.
        // TODO: could that be that we just hit coinbase size limit?
        LogPrintf("%s -- ERROR: Block invalid, too few superblock payments\n", __func__);
        return false;
    }

    // payments should not exceed limit
    CAmount nPaymentsTotalAmount = std23::ranges::fold_left(payments, CAmount{0}, [](CAmount s, const auto& p) { return s + p.nValue; });
    if (nPaymentsTotalAmount > nPaymentsLimit) {
        LogPrintf("%s -- ERROR: Block invalid, payments limit exceeded: payments %lld, limit %lld\n", __func__, nPaymentsTotalAmount, nPaymentsLimit);
        return false;
    }

    // miner and masternodes should not get more than they would usually get
    CAmount nBlockValue = txNew.GetValueOut();
    if (nBlockValue > blockReward + nPaymentsTotalAmount) {
        LogPrintf("%s -- ERROR: Block invalid, block value limit exceeded: block %lld, limit %lld\n", __func__, nBlockValue, blockReward + nPaymentsTotalAmount);
        return false;
    }

    int nVoutIndex = -1;
    for (int i = 0; i < nPayments; i++) {
        const CTxOut& payment = payments[i];
        bool fPaymentMatch = false;

        // From V24 on, start past the previously matched output so each expected
        // payment consumes a distinct vout (two adjacent payments with the same
        // script and amount must match two separate outputs, not the same one
        // twice). Before V24 the scan restarted at the previously matched index
        // (inclusive), which is kept for backwards compatibility.
        // TODO: After V24 is hardened/finalized so historical duplicate-output
        // blocks cannot be encountered, simplify this path to the V24 scan only.
        const int nVoutStart = is_v24 ? nVoutIndex + 1 : std::max(nVoutIndex, 0);
        for (int j = nVoutStart; j < nOutputs; j++) {
            // Find superblock payment
            fPaymentMatch = ((payment.scriptPubKey == txNew.vout[j].scriptPubKey) &&
                             (payment.nValue == txNew.vout[j].nValue));

            if (fPaymentMatch) {
                nVoutIndex = j;
                break;
            }
        }

        if (!fPaymentMatch) {
            // Superblock payment not found!

            CTxDestination dest;
            ExtractDestination(payment.scriptPubKey, dest);
            LogPrintf("%s -- ERROR: Block invalid: %d payment %d to %s not found\n", __func__, i, payment.nValue, EncodeDestination(dest));

            return false;
        }
    }

    return true;
}

bool GetMasternodePayments(const CDeterministicMNList& mn_list, const CBlockIndex* pindexPrev,
                           const CAmount blockSubsidy, const CAmount feeReward, MnRewardEra era,
                           const Consensus::Params& consensus_params,
                           std::vector<CTxOut>& voutMasternodePaymentsRet)
{
    voutMasternodePaymentsRet.clear();

    const int nBlockHeight = pindexPrev  == nullptr ? 0 : pindexPrev->nHeight + 1;

    CAmount masternodeReward = GetMasternodePayment(nBlockHeight, blockSubsidy + feeReward, consensus_params, era);

    // Credit Pool doesn't exist before V20. If any part of reward will re-allocated to credit pool before v20
    // activation these fund will be just permanently lost. Applicable for devnets, regtest, testnet
    if (era == MnRewardEra::EvoReward) {
        CAmount masternodeSubsidyReward = GetMasternodePayment(nBlockHeight, blockSubsidy, consensus_params, era);
        const CAmount platformReward = PlatformShare(masternodeSubsidyReward);
        masternodeReward -= platformReward;

        assert(MoneyRange(masternodeReward));

        LogPrint(BCLog::MNPAYMENTS, "%s -- MN reward %lld reallocated to credit pool\n", __func__, platformReward);
        voutMasternodePaymentsRet.emplace_back(platformReward, CScript() << OP_RETURN);
    }
    if (mn_list.GetCounts().total() == 0) {
        LogPrint(BCLog::MNPAYMENTS, "%s -- no masternode registered to receive a payment\n", __func__);
        return true;
    }
    const auto dmnPayee = mn_list.GetMNPayee(pindexPrev);
    if (!dmnPayee) {
        return false;
    }

    CAmount operatorReward = 0;

    if (dmnPayee->nOperatorReward != 0 && dmnPayee->pdmnState->scriptOperatorPayout != CScript()) {
        // This calculation might eventually turn out to result in 0 even if an operator reward percentage is given.
        // This will however only happen in a few years when the block rewards drops very low.
        operatorReward = (masternodeReward * dmnPayee->nOperatorReward) / 10000;
        masternodeReward -= operatorReward;
    }

    if (dmnPayee->pdmnState->IsShared()) {
        // Shared masternodes split the owner reward by recorded collateral contribution, paying
        // each share's reward script (or its refund script when no reward script is set)
        const auto& shares = dmnPayee->pdmnState->shares;
        const auto amounts = SplitAmountByShares(masternodeReward, shares);
        for (size_t i = 0; i < shares.size(); ++i) {
            if (amounts[i] > 0) {
                voutMasternodePaymentsRet.emplace_back(amounts[i], shares[i].RewardScript());
            }
        }
    } else {
        const auto owner_payouts = GetOwnerPayouts(*dmnPayee->pdmnState);
        CAmount paid_owner_reward{0};
        for (size_t i = 0; i < owner_payouts.size(); ++i) {
            const bool last = i + 1 == owner_payouts.size();
            const CAmount payout_amount = last ? masternodeReward - paid_owner_reward
                                               : (masternodeReward * owner_payouts[i].reward) / MasternodePayoutShare::MAX_REWARD;
            paid_owner_reward += payout_amount;
            if (payout_amount > 0) {
                voutMasternodePaymentsRet.emplace_back(payout_amount, owner_payouts[i].scriptPayout);
            }
        }
    }
    if (operatorReward > 0) {
        voutMasternodePaymentsRet.emplace_back(operatorReward, dmnPayee->pdmnState->scriptOperatorPayout);
    }

    return true;
}
