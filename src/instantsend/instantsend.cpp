// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <instantsend/instantsend.h>

#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <masternode/sync.h>
#include <node/blockstorage.h>
#include <spork.h>
#include <stats/client.h>

using node::fImporting;
using node::fReindex;

namespace llmq {
CInstantSendManager::CInstantSendManager(const chainlock::Chainlocks& chainlocks, CSporkManager& sporkman,
                                         const CMasternodeSync& mn_sync, const util::DbWrapperParams& db_params) :
    db{db_params},
    m_chainlocks{chainlocks},
    spork_manager{sporkman},
    m_mn_sync{mn_sync}
{
}

CInstantSendManager::~CInstantSendManager() = default;

bool ShouldReportISLockTiming() { return g_stats_client->active() || LogAcceptDebug(BCLog::INSTANTSEND); }

void CInstantSendManager::EnqueueInstantSendLock(NodeId from, const uint256& hash,
                                                 std::shared_ptr<instantsend::InstantSendLock> islock)
{
    if (ShouldReportISLockTiming()) {
        auto time_diff = [&]() -> int64_t {
            LOCK(cs_timingsTxSeen);
            if (auto it = timingsTxSeen.find(islock->txid); it != timingsTxSeen.end()) {
                // This is the normal case where we received the TX before the islock
                auto diff = TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()) - it->second;
                timingsTxSeen.erase(it);
                return diff;
            }
            // But if we received the islock and don't know when we got the tx, then say 0, to indicate we received the islock first.
            return 0;
        }();
        ::g_stats_client->timing("islock_ms", time_diff);
        LogPrint(BCLog::INSTANTSEND, "CInstantSendManager::%s -- txid=%s, islock took %dms\n", __func__,
                 islock->txid.ToString(), time_diff);
    }

    LOCK(cs_pendingLocks);
    pendingInstantSendLocks.emplace(hash, instantsend::PendingISLockFromPeer{from, std::move(islock)});
}

instantsend::PendingState CInstantSendManager::FetchPendingLocks()
{
    instantsend::PendingState ret;

    LOCK(cs_pendingLocks);
    // only process a max 32 locks at a time to avoid duplicate verification of recovered signatures which have been
    // verified by CSigningManager in parallel
    const size_t maxCount = 32;
    // The keys of the removed values are temporaily stored here to avoid invalidating an iterator
    std::vector<uint256> removed;
    removed.reserve(std::min(maxCount, pendingInstantSendLocks.size()));

    for (auto& [islockHash, pending] : pendingInstantSendLocks) {
        // Check if we've reached max count
        if (ret.m_pending_is.size() >= maxCount) {
            ret.m_pending_work = true;
            break;
        }
        ret.m_pending_is.push_back(instantsend::PendingISLockEntry{std::move(pending), islockHash});
        removed.emplace_back(islockHash);
    }

    for (const auto& islockHash : removed) {
        pendingInstantSendLocks.erase(islockHash);
    }

    return ret;
}

bool CInstantSendManager::PreVerifyIsLock(const uint256& hash, const instantsend::InstantSendLockPtr& islock, NodeId from) const
{
    if (db.KnownInstantSendLock(hash)) {
        return false;
    }

    if (const auto sameTxIsLock = db.GetInstantSendLockByTxid(islock->txid)) {
        // can happen, nothing to do
        return false;
    }
    for (const auto& in : islock->inputs) {
        const auto sameOutpointIsLock = db.GetInstantSendLockByInput(in);
        if (sameOutpointIsLock != nullptr) {
            LogPrintf("CInstantSendManager::%s -- txid=%s, islock=%s: conflicting outpoint in islock. input=%s, other islock=%s, peer=%d\n", __func__,
                      islock->txid.ToString(), hash.ToString(), in.ToStringShort(), ::SerializeHash(*sameOutpointIsLock).ToString(), from);
        }
    }
    return true;
}

void CInstantSendManager::WriteNewISLock(const uint256& hash, const instantsend::InstantSendLockPtr& islock,
                                         std::optional<int> minedHeight)
{
    db.WriteNewInstantSendLock(hash, islock);
    if (minedHeight.has_value()) {
        db.WriteInstantSendLockMined(hash, *minedHeight);
    }
}

void CInstantSendManager::AddPendingISLock(const uint256& hash, const instantsend::InstantSendLockPtr& islock, NodeId from)
{
    // put it in a separate pending map and try again later
    LOCK(cs_pendingLocks);
    pendingNoTxInstantSendLocks.try_emplace(hash, instantsend::PendingISLockFromPeer{from, islock});
}

void CInstantSendManager::TransactionIsRemoved(const CTransactionRef& tx)
{
    if (tx->vin.empty()) {
        return;
    }

    instantsend::InstantSendLockPtr islock = GetInstantSendLockByTxid(tx->GetHash());

    if (islock == nullptr) {
        return;
    }

    LogPrint(BCLog::INSTANTSEND, "CInstantSendManager::%s -- transaction %s was removed from mempool\n", __func__,
             tx->GetHash().ToString());
    RemoveConflictingLock(::SerializeHash(*islock), *islock);
}

void CInstantSendManager::WriteBlockISLocks(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex)
{
    db.WriteBlockInstantSendLocks(pblock, pindex);
}

void CInstantSendManager::RemoveBlockISLocks(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex)
{
    db.RemoveBlockInstantSendLocks(pblock, pindex);
}

instantsend::InstantSendLockPtr CInstantSendManager::AttachISLockToTx(const CTransactionRef& tx)
{
    instantsend::InstantSendLockPtr ret_islock{nullptr};
    LOCK(cs_pendingLocks);
    auto it = pendingNoTxInstantSendLocks.begin();
    while (it != pendingNoTxInstantSendLocks.end()) {
        if (it->second.islock->txid == tx->GetHash()) {
            // we received an islock earlier, let's put it back into pending and verify/lock
            LogPrint(BCLog::INSTANTSEND, "CInstantSendManager::%s -- txid=%s, islock=%s\n", __func__,
                     tx->GetHash().ToString(), it->first.ToString());
            ret_islock = it->second.islock;
            pendingInstantSendLocks.try_emplace(it->first, it->second);
            pendingNoTxInstantSendLocks.erase(it);
            return ret_islock;
        }
        ++it;
    }
    return ret_islock; // not found, nullptr
}

void CInstantSendManager::AddNonLockedTx(const CTransactionRef& tx, const CBlockIndex* pindexMined)
{
    {
        LOCK(cs_nonLocked);
        auto [it, did_insert] = nonLockedTxs.emplace(tx->GetHash(), NonLockedTxInfo());
        auto& nonLockedTxInfo = it->second;
        nonLockedTxInfo.pindexMined = pindexMined;

        if (did_insert) {
            nonLockedTxInfo.tx = tx;
            for (const auto& in : tx->vin) {
                nonLockedTxs[in.prevout.hash].children.emplace(tx->GetHash());
                nonLockedTxsByOutpoints.emplace(in.prevout, tx->GetHash());
            }
        }
    }
    AttachISLockToTx(tx);
    if (ShouldReportISLockTiming()) {
        LOCK(cs_timingsTxSeen);
        // Only insert the time the first time we see the tx, as we sometimes try to resign
        timingsTxSeen.try_emplace(tx->GetHash(), TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()));
    }

    LogPrint(BCLog::INSTANTSEND, "CInstantSendManager::%s -- txid=%s, pindexMined=%s\n", __func__,
             tx->GetHash().ToString(), pindexMined ? pindexMined->GetBlockHash().ToString() : "");
}

void CInstantSendManager::RemoveNonLockedTx(const uint256& txid, bool retryChildren)
{
    LOCK(cs_nonLocked);

    auto it = nonLockedTxs.find(txid);
    if (it == nonLockedTxs.end()) {
        return;
    }
    const auto& info = it->second;

    size_t retryChildrenCount = 0;
    if (retryChildren) {
        // TX got locked, so we can retry locking children
        LOCK(cs_pendingRetry);
        for (const auto& childTxid : info.children) {
            pendingRetryTxs.emplace(childTxid);
            retryChildrenCount++;
        }
    }
    // don't try to lock it anymore
    WITH_LOCK(cs_pendingRetry, pendingRetryTxs.erase(txid));

    if (info.tx) {
        for (const auto& in : info.tx->vin) {
            if (auto jt = nonLockedTxs.find(in.prevout.hash); jt != nonLockedTxs.end()) {
                jt->second.children.erase(txid);
                if (!jt->second.tx && jt->second.children.empty()) {
                    nonLockedTxs.erase(jt);
                }
            }
            nonLockedTxsByOutpoints.erase(in.prevout);
        }
    }

    nonLockedTxs.erase(it);

    LogPrint(BCLog::INSTANTSEND, "CInstantSendManager::%s -- txid=%s, retryChildren=%d, retryChildrenCount=%d\n",
             __func__, txid.ToString(), retryChildren, retryChildrenCount);
}

std::vector<CTransactionRef> CInstantSendManager::PrepareTxToRetry()
{
    std::vector<CTransactionRef> txns{};

    LOCK2(cs_nonLocked, cs_pendingRetry);
    if (pendingRetryTxs.empty()) return txns;
    txns.reserve(pendingRetryTxs.size());
    for (const auto& txid : pendingRetryTxs) {
        if (auto it = nonLockedTxs.find(txid); it != nonLockedTxs.end()) {
            const auto& [_, tx_info] = *it;
            if (tx_info.tx) {
                txns.push_back(tx_info.tx);
            }
        }
    }
    return txns;
}

void CInstantSendManager::TryEmplacePendingLock(const uint256& hash, const NodeId id,
                                                const instantsend::InstantSendLockPtr& islock)
{
    if (db.KnownInstantSendLock(hash)) return;
    LOCK(cs_pendingLocks);
    if (!pendingInstantSendLocks.count(hash)) {
        pendingInstantSendLocks.emplace(hash, instantsend::PendingISLockFromPeer{id, islock});
    }
}

std::unordered_map<const CBlockIndex*, Uint256HashMap<CTransactionRef>> CInstantSendManager::RetrieveISConflicts(
    const uint256& islockHash, const instantsend::InstantSendLock& islock)
{
    // Lets first collect all non-locked TXs which conflict with the given ISLOCK
    std::unordered_map<const CBlockIndex*, Uint256HashMap<CTransactionRef>> conflicts;
    {
        LOCK(cs_nonLocked);
        for (const auto& in : islock.inputs) {
            auto it = nonLockedTxsByOutpoints.find(in);
            if (it != nonLockedTxsByOutpoints.end()) {
                auto& conflictTxid = it->second;
                if (conflictTxid == islock.txid) {
                    continue;
                }
                auto jt = nonLockedTxs.find(conflictTxid);
                if (jt == nonLockedTxs.end()) {
                    continue;
                }
                auto& info = jt->second;
                if (!info.pindexMined || !info.tx) {
                    continue;
                }
                LogPrintf("CInstantSendManager::%s -- txid=%s, islock=%s: mined TX %s with input %s and mined in block %s conflicts with islock\n", __func__,
                          islock.txid.ToString(), islockHash.ToString(), conflictTxid.ToString(), in.ToStringShort(), info.pindexMined->GetBlockHash().ToString());
                conflicts[info.pindexMined].emplace(conflictTxid, info.tx);
            }
        }
    }

    return conflicts;
}

bool CInstantSendManager::IsKnownTx(const uint256& islockHash) const
{
    LOCK(cs_pendingLocks);
    return pendingNoTxInstantSendLocks.find(islockHash) == pendingNoTxInstantSendLocks.end();
}

void CInstantSendManager::RemoveConflictingLock(const uint256& islockHash, const instantsend::InstantSendLock& islock)
{
    LogPrintf("CInstantSendManager::%s -- txid=%s, islock=%s: Removing ISLOCK and its chained children\n", __func__,
              islock.txid.ToString(), islockHash.ToString());
    const int tipHeight = GetTipHeight();

    auto removedIslocks = db.RemoveChainedInstantSendLocks(islockHash, islock.txid, tipHeight);
    for (const auto& h : removedIslocks) {
        LogPrintf("CInstantSendManager::%s -- txid=%s, islock=%s: removed (child) ISLOCK %s\n", __func__,
                  islock.txid.ToString(), islockHash.ToString(), h.ToString());
    }
}

bool CInstantSendManager::AlreadyHave(const CInv& inv) const
{
    if (!IsInstantSendEnabled()) {
        return true;
    }

    return WITH_LOCK(cs_pendingLocks, return pendingInstantSendLocks.count(inv.hash) != 0 ||
                                             pendingNoTxInstantSendLocks.count(inv.hash) != 0) ||
           db.KnownInstantSendLock(inv.hash);
}

bool CInstantSendManager::GetInstantSendLockByHash(const uint256& hash, instantsend::InstantSendLock& ret) const
{
    if (!IsInstantSendEnabled()) {
        return false;
    }

    auto islock = db.GetInstantSendLockByHash(hash);
    if (!islock) {
        LOCK(cs_pendingLocks);
        auto it = pendingInstantSendLocks.find(hash);
        if (it != pendingInstantSendLocks.end()) {
            islock = it->second.islock;
        } else {
            auto itNoTx = pendingNoTxInstantSendLocks.find(hash);
            if (itNoTx != pendingNoTxInstantSendLocks.end()) {
                islock = itNoTx->second.islock;
            } else {
                return false;
            }
        }
    }
    ret = *islock;
    return true;
}

instantsend::InstantSendLockPtr CInstantSendManager::GetInstantSendLockByTxid(const uint256& txid) const
{
    if (!IsInstantSendEnabled()) {
        return nullptr;
    }

    return db.GetInstantSendLockByTxid(txid);
}

bool CInstantSendManager::IsLocked(const uint256& txHash) const
{
    if (!IsInstantSendEnabled()) {
        return false;
    }

    return db.KnownInstantSendLock(db.GetInstantSendLockHashByTxid(txHash));
}

bool CInstantSendManager::IsWaitingForTx(const uint256& txHash) const
{
    if (!IsInstantSendEnabled()) {
        return false;
    }

    LOCK(cs_pendingLocks);
    auto it = pendingNoTxInstantSendLocks.begin();
    while (it != pendingNoTxInstantSendLocks.end()) {
        if (it->second.islock->txid == txHash) {
            LogPrint(BCLog::INSTANTSEND, "CInstantSendManager::%s -- txid=%s, islock=%s\n", __func__, txHash.ToString(),
                     it->first.ToString());
            return true;
        }
        ++it;
    }
    return false;
}

instantsend::InstantSendLockPtr CInstantSendManager::GetConflictingLock(const CTransaction& tx) const
{
    if (!IsInstantSendEnabled()) {
        return nullptr;
    }

    for (const auto& in : tx.vin) {
        auto otherIsLock = db.GetInstantSendLockByInput(in.prevout);
        if (!otherIsLock) {
            continue;
        }

        if (otherIsLock->txid != tx.GetHash()) {
            return otherIsLock;
        }
    }
    return nullptr;
}

size_t CInstantSendManager::GetInstantSendLockCount() const
{
    return db.GetInstantSendLockCount();
}

CInstantSendManager::Counts CInstantSendManager::GetCounts() const
{
    Counts ret;
    ret.m_verified = db.GetInstantSendLockCount();
    {
        LOCK(cs_pendingLocks);
        ret.m_unverified = pendingInstantSendLocks.size();
        ret.m_awaiting_tx = pendingNoTxInstantSendLocks.size();
    }
    {
        LOCK(cs_nonLocked);
        ret.m_unprotected_tx = nonLockedTxs.size();
    }
    return ret;
}

void CInstantSendManager::CacheBlockHeight(const CBlockIndex* const block_index) const
{
    LOCK(cs_height_cache);
    m_cached_block_heights.insert(block_index->GetBlockHash(), block_index->nHeight);
}

void CInstantSendManager::CacheDisconnectBlock(const CBlockIndex* pindexDisconnected)
{
    LOCK(cs_height_cache);
    m_cached_block_heights.erase(pindexDisconnected->GetBlockHash());
}

std::optional<int> CInstantSendManager::GetCachedHeight(const uint256& hash) const
{
    LOCK(cs_height_cache);

    int cached_height{0};
    if (m_cached_block_heights.get(hash, cached_height)) return cached_height;

    return std::nullopt;
}

void CInstantSendManager::CacheTipHeight(const CBlockIndex* const tip) const
{
    LOCK(cs_height_cache);
    if (tip) {
        m_cached_block_heights.insert(tip->GetBlockHash(), tip->nHeight);
        m_cached_tip_height = tip->nHeight;
    } else {
        m_cached_tip_height = -1;
    }
}

int CInstantSendManager::GetTipHeight() const
{
    // It returns the cached tip height which is updated through notification mechanism
    // If cached tip is not set by any reason, it's okay to return 0 because
    // chainstate is not fully loaded yet and tip is not set
    LOCK(cs_height_cache);
    if (m_cached_tip_height >= 0) {
        return m_cached_tip_height;
    }
    return 0;
}

bool CInstantSendManager::IsInstantSendEnabled() const
{
    return !fReindex && !fImporting && spork_manager.IsSporkActive(SPORK_2_INSTANTSEND_ENABLED);
}

bool CInstantSendManager::RejectConflictingBlocks() const
{
    if (!m_mn_sync.IsBlockchainSynced()) {
        return false;
    }
    if (!spork_manager.IsSporkActive(SPORK_3_INSTANTSEND_BLOCK_FILTERING)) {
        LogPrint(BCLog::INSTANTSEND, "%s: spork3 is off, skipping transaction locking checks\n", __func__);
        return false;
    }
    return true;
}

Uint256HashMap<instantsend::InstantSendLockPtr> CInstantSendManager::RemoveConfirmedInstantSendLocks(const CBlockIndex* pindex)
{
    int nUntilHeight = pindex->nHeight;
    auto removeISLocks = db.RemoveConfirmedInstantSendLocks(nUntilHeight);

    db.RemoveArchivedInstantSendLocks(nUntilHeight - 100);

    // Find all previously unlocked TXs that got locked by this fully confirmed (ChainLock) block and remove them
    // from the nonLockedTxs map. Also collect all children of these TXs and mark them for retrying of IS locking.
    std::vector<uint256> toRemove;
    {
        LOCK(cs_nonLocked);
        for (const auto& p : nonLockedTxs) {
            const auto* pindexMined = p.second.pindexMined;

            if (pindexMined && pindex->GetAncestor(pindexMined->nHeight) == pindexMined) {
                toRemove.emplace_back(p.first);
            }
        }
    }
    for (const auto& txid : toRemove) {
        // This will also add children to pendingRetryTxs
        RemoveNonLockedTx(txid, true);
    }

    return removeISLocks;
}
} // namespace llmq
