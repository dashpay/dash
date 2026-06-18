// Copyright (c) 2024-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/chainhelper.h>

#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <evo/creditpool.h>
#include <evo/mnhftx.h>
#include <evo/specialtxman.h>
#include <governance/superblock.h>
#include <instantsend/instantsend.h>
#include <instantsend/lock.h>
#include <logging.h>
#include <masternode/payments.h>
#include <masternode/sync.h>

CChainstateHelper::CChainstateHelper(CEvoDB& evodb, CDeterministicMNManager& dmnman, const CMasternodeSync& mn_sync,
                                     llmq::CInstantSendManager& isman, llmq::CQuorumBlockProcessor& qblockman,
                                     llmq::CQuorumSnapshotManager& qsnapman, const ChainstateManager& chainman,
                                     const Consensus::Params& consensus_params, const chainlock::Chainlocks& chainlocks,
                                     const llmq::CQuorumManager& qman) :
    isman{isman},
    mn_sync{mn_sync},
    credit_pool_manager{std::make_unique<CCreditPoolManager>(evodb, chainman)},
    m_chainlocks{chainlocks},
    ehf_manager{std::make_unique<CMNHFManager>(evodb, chainman)},
    superblocks{std::make_unique<governance::SuperblockManager>()},
    mn_payments{std::make_unique<CMNPaymentsProcessor>(dmnman, *superblocks, chainman, consensus_params)},
    special_tx{std::make_unique<CSpecialTxProcessor>(*credit_pool_manager, dmnman, *ehf_manager, qblockman, qsnapman,
                                                     chainman, consensus_params, chainlocks, qman)}
{}

CChainstateHelper::~CChainstateHelper() = default;

bool CChainstateHelper::IsSuperblockValidationRequired(const CBlockIndex* const pindex)
{
    if (m_chainlocks.GetBestChainLockHeight() >= pindex->nHeight) {
        LogPrint(BCLog::MNPAYMENTS, "%s -- validation of chainlocked block=%s is skipped\n", __func__, pindex->GetBlockHash().ToString());
        return false;
    }
    if (!mn_sync.IsSynced()) {
        LogPrint(BCLog::MNPAYMENTS, "%s -- WARNING! Node is not fully synced, checked superblock for block=%s max bounds only\n", __func__, pindex->GetBlockHash().ToString());
        return false;
    }
    return true;
}

/** Passthrough functions to chainlock::Chainlocks */
bool CChainstateHelper::HasConflictingChainLock(int nHeight, const uint256& blockHash) const
{
    return m_chainlocks.HasConflictingChainLock(nHeight, blockHash);
}

bool CChainstateHelper::HasChainLock(int nHeight, const uint256& blockHash) const
{
    return m_chainlocks.HasChainLock(nHeight, blockHash);
}

int32_t CChainstateHelper::GetBestChainLockHeight() const { return m_chainlocks.GetBestChainLockHeight(); }

/** Passthrough functions to CCreditPoolManager */
CCreditPool CChainstateHelper::GetCreditPool(const CBlockIndex* const pindex)
{
    return credit_pool_manager->GetCreditPool(pindex);
}

/** Passthrough functions to CInstantSendManager */
std::optional<std::pair</*islock_hash=*/uint256, /*txid=*/uint256>> CChainstateHelper::ConflictingISLockIfAny(
    const CTransaction& tx) const
{
    const auto islock = isman.GetConflictingLock(tx);
    if (!islock) return std::nullopt;
    return std::make_pair(::SerializeHash(*islock), islock->txid);
}

bool CChainstateHelper::IsInstantSendWaitingForTx(const uint256& hash) const { return isman.IsWaitingForTx(hash); }

bool CChainstateHelper::RemoveConflictingISLockByTx(const CTransaction& tx)
{
    const auto islock = isman.GetConflictingLock(tx);
    if (!islock) return false;
    isman.RemoveConflictingLock(::SerializeHash(*islock), *islock);
    return true;
}

std::unordered_map<uint8_t, int> CChainstateHelper::GetSignalsStage(const CBlockIndex* const pindexPrev)
{
    return ehf_manager->GetSignalsStage(pindexPrev);
}
