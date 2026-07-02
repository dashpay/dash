// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MASTERNODE_PAYMENTS_H
#define BITCOIN_MASTERNODE_PAYMENTS_H

#include <consensus/amount.h>

#include <string>
#include <vector>

class CBlock;
class CBlockIndex;
class CChain;
class CDeterministicMNManager;
class CTransaction;
class CTxOut;

/**
 * Match the list of expected masternode payment outputs against the coinbase
 * outputs.
 *
 * When @p strict_multiplicity is true, every expected output must be matched
 * by a distinct actual output (multiplicity-correct matching): two identical
 * expected outputs require two identical actual outputs.
 *
 * When false, the legacy behaviour is used where each expected output only
 * has to appear at least once in the actual outputs. This is preserved for
 * pre-v24 historical validation.
 *
 * @return -1 if every expected output is matched, otherwise the index in
 * @p expected of the first output that could not be matched.
 */
int FindUnmatchedMasternodePayment(const std::vector<CTxOut>& expected,
                                   const std::vector<CTxOut>& actual,
                                   bool strict_multiplicity);

struct CMutableTransaction;

namespace governance {
class SuperblockManager;
}
namespace Consensus { struct Params; }

/**
 * This helper returns amount that should be reallocated to platform
 * It is calculated based on total amount of masternode rewards (not block reward)
 */
CAmount PlatformShare(const CAmount masternodeReward);

/**
 * Masternode reward era for a given block, gating how the reward is computed.
 * Each era implies the previous one: EvoReward is only reachable once CreditPool
 * (V20) is active, so the ordering encodes that invariant. The era is computed by
 * the caller (which owns the block context) and passed in, keeping this module free
 * of deployment dependencies. Note this is orthogonal to DIP0003 enforcement, which
 * gates whether payees are validated at all and is handled separately.
 */
enum class MnRewardEra {
    Classic,    // historical reward schedule, no credit pool
    CreditPool, // V20: credit pool active, no platform reallocation yet
    EvoReward,  // MN_RR: platform share is reallocated from the masternode reward
};

enum class SuperBlockCheckType {
    NoCheck, // for chainlocked blocks or during sync
    AllowDuplicates,
    DisallowDuplicates,
};

CAmount GetMasternodePayment(int nHeight, CAmount blockValue, const Consensus::Params& consensus_params, MnRewardEra era);

class CMNPaymentsProcessor
{
private:
    CDeterministicMNManager& m_dmnman;
    governance::SuperblockManager& m_superblocks;
    const Consensus::Params& m_consensus_params;

private:
    [[nodiscard]] bool GetBlockTxOuts(const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward,
                                      MnRewardEra era, std::vector<CTxOut>& voutMasternodePaymentsRet);
    [[nodiscard]] bool GetMasternodeTxOuts(const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward,
                                      MnRewardEra era, std::vector<CTxOut>& voutMasternodePaymentsRet);
    [[nodiscard]] bool IsTransactionValid(const CTransaction& txNew, const CBlockIndex* pindexPrev, const CAmount blockSubsidy,
                                          const CAmount feeReward, MnRewardEra era);
    [[nodiscard]] bool IsOldBudgetBlockValueValid(const CBlock& block, const int nBlockHeight, const CAmount blockReward, std::string& strErrorRet);

public:
    explicit CMNPaymentsProcessor(CDeterministicMNManager& dmnman, governance::SuperblockManager& superblocks,
                                  const Consensus::Params& consensus_params) :
        m_dmnman{dmnman},
        m_superblocks{superblocks},
        m_consensus_params{consensus_params}
    {
    }

    bool IsBlockValueValid(const CChain& active_chain, const CBlock& block, const CBlockIndex* pindexPrev, const CAmount blockReward, std::string& strErrorRet, SuperBlockCheckType check_superblock);
    bool IsBlockPayeeValid(const CChain& active_chain, const CTransaction& txNew, const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward, MnRewardEra era, SuperBlockCheckType check_superblock);
    void FillBlockPayments(CMutableTransaction& txNew, const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward,
                           MnRewardEra era, std::vector<CTxOut>& voutMasternodePaymentsRet, std::vector<CTxOut>& voutSuperblockPaymentsRet);
};

#endif // BITCOIN_MASTERNODE_PAYMENTS_H
