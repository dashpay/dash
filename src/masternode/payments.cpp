// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/payments.h>

#include <evo/deterministicmns.h>
#include <governance/superblock.h>
#include <masternode/sync.h>

#include <chain.h>
#include <consensus/amount.h>
#include <deploymentstatus.h>
#include <key_io.h>
#include <logging.h>
#include <primitives/block.h>
#include <script/standard.h>
#include <tinyformat.h>

#include <cassert>
#include <ranges>
#include <string>

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

[[nodiscard]] bool CMNPaymentsProcessor::GetBlockTxOuts(const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward,
                                                        MnRewardEra era, std::vector<CTxOut>& voutMasternodePaymentsRet)
{
    voutMasternodePaymentsRet.clear();

    const int nBlockHeight = pindexPrev  == nullptr ? 0 : pindexPrev->nHeight + 1;

    CAmount masternodeReward = GetMasternodePayment(nBlockHeight, blockSubsidy + feeReward, m_consensus_params, era);

    // Credit Pool doesn't exist before V20. If any part of reward will re-allocated to credit pool before v20
    // activation these fund will be just permanently lost. Applicable for devnets, regtest, testnet
    if (era == MnRewardEra::EvoReward) {
        CAmount masternodeSubsidyReward = GetMasternodePayment(nBlockHeight, blockSubsidy, m_consensus_params, era);
        const CAmount platformReward = PlatformShare(masternodeSubsidyReward);
        masternodeReward -= platformReward;

        assert(MoneyRange(masternodeReward));

        LogPrint(BCLog::MNPAYMENTS, "CMNPaymentsProcessor::%s -- MN reward %lld reallocated to credit pool\n", __func__, platformReward);
        voutMasternodePaymentsRet.emplace_back(platformReward, CScript() << OP_RETURN);
    }
    const auto mnList = m_dmnman.GetListForBlock(pindexPrev);
    if (mnList.GetCounts().total() == 0) {
        LogPrint(BCLog::MNPAYMENTS, "CMNPaymentsProcessor::%s -- no masternode registered to receive a payment\n", __func__);
        return true;
    }
    const auto dmnPayee = mnList.GetMNPayee(pindexPrev);
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

    const auto owner_payouts = GetOwnerPayouts(dmnPayee->pdmnState->nVersion, dmnPayee->pdmnState->scriptPayout,
                                               dmnPayee->pdmnState->payouts);
    CAmount paid_owner_reward{0};
    for (size_t i = 0; i < owner_payouts.size(); ++i) {
        const bool last = i + 1 == owner_payouts.size();
        const CAmount payout_amount = last ? masternodeReward - paid_owner_reward
                                           : (masternodeReward * owner_payouts[i].reward) / CMasternodePayoutShare::MAX_REWARD;
        paid_owner_reward += payout_amount;
        if (payout_amount > 0) {
            voutMasternodePaymentsRet.emplace_back(payout_amount, owner_payouts[i].scriptPayout);
        }
    }
    if (operatorReward > 0) {
        voutMasternodePaymentsRet.emplace_back(operatorReward, dmnPayee->pdmnState->scriptOperatorPayout);
    }

    return true;
}

/**
*   GetMasternodeTxOuts
*
*   Get masternode payment tx outputs
*/
[[nodiscard]] bool CMNPaymentsProcessor::GetMasternodeTxOuts(const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward,
                                                             MnRewardEra era, std::vector<CTxOut>& voutMasternodePaymentsRet)
{
    // make sure it's not filled yet
    voutMasternodePaymentsRet.clear();

    if(!GetBlockTxOuts(pindexPrev, blockSubsidy, feeReward, era, voutMasternodePaymentsRet)) {
        LogPrintf("CMNPaymentsProcessor::%s -- ERROR Failed to get payee\n", __func__);
        return false;
    }

    for (const auto& txout : voutMasternodePaymentsRet) {
        CTxDestination dest;
        ExtractDestination(txout.scriptPubKey, dest);

        LogPrintf("CMNPaymentsProcessor::%s -- Masternode payment %lld to %s\n", __func__, txout.nValue, EncodeDestination(dest));
    }

    return true;
}

[[nodiscard]] bool CMNPaymentsProcessor::IsTransactionValid(const CTransaction& txNew, const CBlockIndex* pindexPrev, const CAmount blockSubsidy,
                                                            const CAmount feeReward, MnRewardEra era)
{
    const int nBlockHeight = pindexPrev  == nullptr ? 0 : pindexPrev->nHeight + 1;
    if (!DeploymentDIP0003Enforced(nBlockHeight, m_consensus_params)) {
        // can't verify historical blocks here
        return true;
    }

    std::vector<CTxOut> voutMasternodePayments;
    if (!GetBlockTxOuts(pindexPrev, blockSubsidy, feeReward, era, voutMasternodePayments)) {
        LogPrintf("CMNPaymentsProcessor::%s -- ERROR! Failed to get payees for block at height %s\n", __func__, nBlockHeight);
        return true;
    }

    // After v24 activation each expected payment must be matched by a distinct coinbase output:
    // duplicate expected outputs require duplicate coinbase outputs. Pre-v24 retains the legacy
    // existence-only check to avoid tightening historical validation.
    const bool strict_match{DeploymentActiveAfter(pindexPrev, m_chainman, Consensus::DEPLOYMENT_V24)};
    const int unmatched_idx = FindUnmatchedMasternodePayment(voutMasternodePayments, txNew.vout, strict_match);
    if (unmatched_idx >= 0) {
        const auto& txout = voutMasternodePayments[unmatched_idx];
        std::string str_payout;
        if (CTxDestination dest; ExtractDestination(txout.scriptPubKey, dest)) {
            str_payout = "address=" + EncodeDestination(dest);
        } else {
            str_payout = "scriptPubKey=" + HexStr(txout.scriptPubKey);
        }
        LogPrintf("CMNPaymentsProcessor::%s -- ERROR! Failed to find expected payee %s amount=%lld height=%d\n",
                  __func__, str_payout, txout.nValue, nBlockHeight);
        return false;
    }
    return true;
}

[[nodiscard]] bool CMNPaymentsProcessor::IsOldBudgetBlockValueValid(const CBlock& block, const int nBlockHeight, const CAmount blockReward, std::string& strErrorRet)
{
    bool isBlockRewardValueMet = (block.vtx[0]->GetValueOut() <= blockReward);

    if (nBlockHeight < m_consensus_params.nBudgetPaymentsStartBlock) {
        strErrorRet = strprintf("Incorrect block %d, old budgets are not activated yet", nBlockHeight);
        return false;
    }

    if (nBlockHeight >= m_consensus_params.nSuperblockStartBlock) {
        strErrorRet = strprintf("Incorrect block %d, old budgets are no longer active", nBlockHeight);
        return false;
    }

    // we are still using budgets, but we have no data about them anymore,
    // all we know is predefined budget cycle and window

    int nOffset = nBlockHeight % m_consensus_params.nBudgetPaymentsCycleBlocks;
    if (nOffset < m_consensus_params.nBudgetPaymentsWindowBlocks) {
        // NOTE: old budget system is disabled since 12.1
        // no old budget blocks should be accepted here on mainnet,
        // testnet/devnet/regtest should produce regular blocks only
        if(!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, old budgets are disabled",
                                    nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
        }
        return isBlockRewardValueMet;
    }
    if(!isBlockRewardValueMet) {
        strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, block is not in old budget cycle window",
                                nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
    }
    return isBlockRewardValueMet;
}

/**
* IsBlockValueValid
*
*   Determine if coinbase outgoing created money is the correct value
*
*   Why is this needed?
*   - In Dash some blocks are superblocks, which output much higher amounts of coins
*   - Other blocks are 10% lower in outgoing value, so in total, no extra coins are created
*   - When non-superblocks are detected, the normal schedule should be maintained
*/
bool CMNPaymentsProcessor::IsBlockValueValid(const CChain& active_chain, const CBlock& block, const CBlockIndex* pindexPrev, const CAmount blockReward, std::string& strErrorRet, SuperBlockCheckType check_superblock)
{
    const int nBlockHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;
    bool isBlockRewardValueMet = (block.vtx[0]->GetValueOut() <= blockReward);

    strErrorRet = "";

    if (nBlockHeight < m_consensus_params.nBudgetPaymentsStartBlock) {
        // old budget system is not activated yet, just make sure we do not exceed the regular block reward
        if(!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, old budgets are not activated yet",
                                    nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
        }
        return isBlockRewardValueMet;
    } else if (nBlockHeight < m_consensus_params.nSuperblockStartBlock) {
        // superblocks are not enabled yet, check if we can pass old budget rules
        return IsOldBudgetBlockValueValid(block, nBlockHeight, blockReward, strErrorRet);
    }

    LogPrint(BCLog::MNPAYMENTS, "block.vtx[0]->GetValueOut() %lld <= blockReward %lld\n", block.vtx[0]->GetValueOut(), blockReward);

    CAmount nSuperblockMaxValue =  blockReward + CSuperblock::GetPaymentsLimit(active_chain, nBlockHeight);
    bool isSuperblockMaxValueMet = (block.vtx[0]->GetValueOut() <= nSuperblockMaxValue);

    LogPrint(BCLog::GOBJECT, "block.vtx[0]->GetValueOut() %lld <= nSuperblockMaxValue %lld\n", block.vtx[0]->GetValueOut(), nSuperblockMaxValue);

    if (!CSuperblock::IsValidBlockHeight(nBlockHeight)) {
        // can't possibly be a superblock, so lets just check for block reward limits
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, only regular blocks are allowed at this height",
                                    nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
        }
        return isBlockRewardValueMet;
    }

    // bail out in case superblock limits were exceeded
    if (!isSuperblockMaxValueMet) {
        strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded superblock max value",
                                nBlockHeight, block.vtx[0]->GetValueOut(), nSuperblockMaxValue);
        return false;
    }

    if (!m_superblocks.IsValid()) {
        LogPrint(BCLog::MNPAYMENTS, "CMNPaymentsProcessor::%s -- WARNING! Not enough data, checked superblock max bounds only\n", __func__);
        // not enough data for full checks but at least we know that the superblock limits were honored.
        // We rely on the network to have followed the correct chain in this case
        return true;
    }

    if (check_superblock == SuperBlockCheckType::NoCheck) return true;

    // we are synced and possibly on a superblock now

    const auto tip_mn_list = m_dmnman.GetListAtChainTip();

    if (!m_superblocks.IsSuperblockTriggered(tip_mn_list, nBlockHeight)) {
        // we are on a valid superblock height but a superblock was not triggered
        // revert to block reward limits in this case
        if(!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, no triggered superblock detected",
                                    nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
        }
        return isBlockRewardValueMet;
    }

    // this actually also checks for correct payees and not only amount
    const bool is_v24{check_superblock == SuperBlockCheckType::DisallowDuplicates};
    if (!m_superblocks.IsValidSuperblock(active_chain, tip_mn_list, *block.vtx[0], nBlockHeight, blockReward, is_v24)) {
        // triggered but invalid? that's weird
        LogPrintf("CMNPaymentsProcessor::%s -- ERROR! Invalid superblock detected at height %d: %s", __func__, nBlockHeight, block.vtx[0]->ToString()); /* Continued */
        // should NOT allow invalid superblocks, when superblocks are enabled
        strErrorRet = strprintf("invalid superblock detected at height %d", nBlockHeight);
        return false;
    }

    // we got a valid superblock
    return true;
}

bool CMNPaymentsProcessor::IsBlockPayeeValid(const CChain& active_chain, const CTransaction& txNew, const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward, MnRewardEra era, SuperBlockCheckType check_superblock)
{
    const int nBlockHeight = pindexPrev  == nullptr ? 0 : pindexPrev->nHeight + 1;

    // Check for correct masternode payment
    if (IsTransactionValid(txNew, pindexPrev, blockSubsidy, feeReward, era)) {
        LogPrint(BCLog::MNPAYMENTS, "CMNPaymentsProcessor::%s -- Valid masternode payment at height %d: %s", __func__, nBlockHeight, txNew.ToString()); /* Continued */
    } else {
        LogPrintf("CMNPaymentsProcessor::%s -- ERROR! Invalid masternode payment detected at height %d: %s", __func__, nBlockHeight, txNew.ToString()); /* Continued */
        return false;
    }

    if (!m_superblocks.IsValid()) {
        // governance data is either incomplete or non-existent
        LogPrint(BCLog::MNPAYMENTS, "CMNPaymentsProcessor::%s -- WARNING! Not enough data, skipping superblock payee checks\n", __func__);
        return true;  // not an error
    }

    if (nBlockHeight < m_consensus_params.nSuperblockStartBlock) {
        // We are still using budgets, but we have no data about them anymore,
        // we can only check masternode payments.
        // NOTE: old budget system is disabled since 12.1 and we should never enter this branch
        // anymore when sync is finished (on mainnet). We have no old budget data but these blocks
        // have tons of confirmations and can be safely accepted without payee verification
        LogPrint(BCLog::GOBJECT, "CMNPaymentsProcessor::%s -- WARNING! Client synced but old budget system is disabled, accepting any payee\n", __func__);
        return true; // not an error
    }

    // superblocks started
    if (check_superblock == SuperBlockCheckType::NoCheck) return true;

    const auto tip_mn_list = m_dmnman.GetListAtChainTip();
    const bool is_v24{check_superblock == SuperBlockCheckType::DisallowDuplicates};
    if (m_superblocks.IsSuperblockTriggered(tip_mn_list, nBlockHeight)) {
        if (m_superblocks.IsValidSuperblock(active_chain, tip_mn_list, txNew, nBlockHeight,
                                            blockSubsidy + feeReward, is_v24)) {
            LogPrint(BCLog::GOBJECT, "CMNPaymentsProcessor::%s -- Valid superblock at height %d: %s", /* Continued */
                     __func__, nBlockHeight, txNew.ToString());
            // continue validation, should also pay MN
        } else {
            LogPrintf("CMNPaymentsProcessor::%s -- ERROR! Invalid superblock detected at height %d: %s", /* Continued */
                      __func__, nBlockHeight, txNew.ToString());
            return false;
        }
    } else {
        LogPrint(BCLog::GOBJECT, "CMNPaymentsProcessor::%s -- No triggered superblock detected at height %d\n",
                 __func__, nBlockHeight);
    }

    return true;
}

void CMNPaymentsProcessor::FillBlockPayments(CMutableTransaction& txNew, const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward,
                                             MnRewardEra era, std::vector<CTxOut>& voutMasternodePaymentsRet, std::vector<CTxOut>& voutSuperblockPaymentsRet)
{
    int nBlockHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;

    // Only create superblocks when one is actually triggered.
    const auto tip_mn_list = m_dmnman.GetListAtChainTip();
    if (m_superblocks.IsSuperblockTriggered(tip_mn_list, nBlockHeight)) {
        LogPrint(BCLog::GOBJECT, "CMNPaymentsProcessor::%s -- Triggered superblock creation at height %d\n", __func__, nBlockHeight);
        m_superblocks.GetSuperblockPayments(tip_mn_list, nBlockHeight, voutSuperblockPaymentsRet);
    }

    if (!GetMasternodeTxOuts(pindexPrev, blockSubsidy, feeReward, era, voutMasternodePaymentsRet)) {
        LogPrint(BCLog::MNPAYMENTS, "CMNPaymentsProcessor::%s -- No masternode to pay (MN list probably empty)\n", __func__);
    }

    txNew.vout.insert(txNew.vout.end(), voutMasternodePaymentsRet.begin(), voutMasternodePaymentsRet.end());
    txNew.vout.insert(txNew.vout.end(), voutSuperblockPaymentsRet.begin(), voutSuperblockPaymentsRet.end());

    std::string voutMasternodeStr;
    for (const auto& txout : voutMasternodePaymentsRet) {
        // subtract MN payment from miner reward
        txNew.vout[0].nValue -= txout.nValue;
        if (!voutMasternodeStr.empty())
            voutMasternodeStr += ",";
        voutMasternodeStr += txout.ToString();
    }

    LogPrint(BCLog::MNPAYMENTS, "CMNPaymentsProcessor::%s -- nBlockHeight %d blockReward %lld voutMasternodePaymentsRet \"%s\" txNew %s", __func__, /* Continued */
                            nBlockHeight, blockSubsidy + feeReward, voutMasternodeStr, txNew.ToString());
}
