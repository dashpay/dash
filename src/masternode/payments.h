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
class CDeterministicMNManager;
class ChainstateManager;
class CTransaction;
class CTxOut;

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

CAmount GetMasternodePayment(int nHeight, CAmount blockValue, MnRewardEra era);

class CMNPaymentsProcessor
{
private:
    CDeterministicMNManager& m_dmnman;
    governance::SuperblockManager& m_superblocks;
    const ChainstateManager& m_chainman;
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
                                  const ChainstateManager& chainman, const Consensus::Params& consensus_params) :
        m_dmnman{dmnman},
        m_superblocks{superblocks},
        m_chainman{chainman},
        m_consensus_params{consensus_params}
    {
    }

    bool IsBlockValueValid(const CBlock& block, const CBlockIndex* pindexPrev, const CAmount blockReward, std::string& strErrorRet, const bool check_superblock);
    bool IsBlockPayeeValid(const CTransaction& txNew, const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward, MnRewardEra era, const bool check_superblock);
    void FillBlockPayments(CMutableTransaction& txNew, const CBlockIndex* pindexPrev, const CAmount blockSubsidy, const CAmount feeReward,
                           MnRewardEra era, std::vector<CTxOut>& voutMasternodePaymentsRet, std::vector<CTxOut>& voutSuperblockPaymentsRet);
};

#endif // BITCOIN_MASTERNODE_PAYMENTS_H
