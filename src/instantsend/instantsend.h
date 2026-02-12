// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INSTANTSEND_INSTANTSEND_H
#define BITCOIN_INSTANTSEND_INSTANTSEND_H

#include <instantsend/db.h>
#include <instantsend/lock.h>

#include <net_types.h>
#include <primitives/transaction.h>
#include <protocol.h>
#include <sync.h>
#include <threadsafety.h>
#include <unordered_lru_cache.h>

#include <optional>
#include <unordered_set>
#include <vector>

#include <saltedhasher.h>

class CBlockIndex;
class CDataStream;
class CMasternodeSync;
class CSporkManager;
namespace Consensus {
struct LLMQParams;
} // namespace Consensus
namespace util {
struct DbWrapperParams;
} // namespace util

namespace chainlock {
class Chainlocks;
}

namespace instantsend {

struct PendingISLockFromPeer {
    NodeId node_id;
    InstantSendLockPtr islock;
};

struct PendingISLockEntry : PendingISLockFromPeer {
    uint256 islock_hash;
};

struct PendingState {
    bool m_pending_work{false};
    std::vector<PendingISLockEntry> m_pending_is;
};
} // namespace instantsend

namespace llmq {
class CSigningManager;

class CInstantSendManager
{
private:
    instantsend::CInstantSendDb db;

    const chainlock::Chainlocks& m_chainlocks;
    CSigningManager& sigman;
    CSporkManager& spork_manager;
    const CMasternodeSync& m_mn_sync;

    mutable Mutex cs_pendingLocks;
    // Incoming and not verified yet
    Uint256HashMap<instantsend::PendingISLockFromPeer> pendingInstantSendLocks GUARDED_BY(cs_pendingLocks);
    // Tried to verify but there is no tx yet
    Uint256HashMap<instantsend::PendingISLockFromPeer> pendingNoTxInstantSendLocks GUARDED_BY(cs_pendingLocks);

    // TXs which are neither IS locked nor ChainLocked. We use this to determine for which TXs we need to retry IS
    // locking of child TXs
    struct NonLockedTxInfo {
        const CBlockIndex* pindexMined;
        CTransactionRef tx;
        Uint256HashSet children;
    };

    mutable Mutex cs_nonLocked;
    Uint256HashMap<NonLockedTxInfo> nonLockedTxs GUARDED_BY(cs_nonLocked);
    std::unordered_map<COutPoint, uint256, SaltedOutpointHasher> nonLockedTxsByOutpoints GUARDED_BY(cs_nonLocked);

    mutable Mutex cs_pendingRetry;
    Uint256HashSet pendingRetryTxs GUARDED_BY(cs_pendingRetry);

    mutable Mutex cs_timingsTxSeen;
    Uint256HashMap<int64_t> timingsTxSeen GUARDED_BY(cs_timingsTxSeen);

    mutable Mutex cs_height_cache;
    static constexpr size_t MAX_BLOCK_HEIGHT_CACHE{16384};
    mutable unordered_lru_cache<uint256, int, StaticSaltedHasher, MAX_BLOCK_HEIGHT_CACHE> m_cached_block_heights
        GUARDED_BY(cs_height_cache);
    mutable int m_cached_tip_height GUARDED_BY(cs_height_cache){-1};

public:
    CInstantSendManager() = delete;
    CInstantSendManager(const CInstantSendManager&) = delete;
    CInstantSendManager& operator=(const CInstantSendManager&) = delete;
    explicit CInstantSendManager(const chainlock::Chainlocks& chainlocks, CSigningManager& _sigman, CSporkManager& sporkman,
                                 const CMasternodeSync& mn_sync, const util::DbWrapperParams& db_params);
    ~CInstantSendManager();

    void AddNonLockedTx(const CTransactionRef& tx, const CBlockIndex* pindexMined)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_nonLocked, !cs_pendingLocks, !cs_timingsTxSeen);
    void RemoveNonLockedTx(const uint256& txid, bool retryChildren)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_nonLocked, !cs_pendingRetry);

    instantsend::InstantSendLockPtr AttachISLockToTx(const CTransactionRef& tx) EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);

    std::unordered_map<const CBlockIndex*, Uint256HashMap<CTransactionRef>> RetrieveISConflicts(
        const uint256& islockHash, const instantsend::InstantSendLock& islock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_nonLocked, !cs_pendingLocks);
    bool IsKnownTx(const uint256& islockHash) const EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);

    bool IsLocked(const uint256& txHash) const;
    bool IsWaitingForTx(const uint256& txHash) const EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);
    instantsend::InstantSendLockPtr GetConflictingLock(const CTransaction& tx) const;

    /* Helpers for communications between CInstantSendManager & NetInstantSend */
    // This helper returns up to 32 pending locks and remove them from queue of pending
    [[nodiscard]] instantsend::PendingState FetchPendingLocks() EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);
    void EnqueueInstantSendLock(NodeId from, const uint256& hash, std::shared_ptr<instantsend::InstantSendLock> islock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);
    [[nodiscard]] std::vector<CTransactionRef> PrepareTxToRetry()
        EXCLUSIVE_LOCKS_REQUIRED(!cs_nonLocked, !cs_pendingRetry);
    CSigningManager& Sigman() { return sigman; }
    const chainlock::Chainlocks& Chainlocks() { return m_chainlocks; }

    void RemoveBlockISLocks(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex);
    void WriteBlockISLocks(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex);
    void WriteNewISLock(const uint256& hash, const instantsend::InstantSendLockPtr& islock, std::optional<int> minedHeight);
    void AddPendingISLock(const uint256& hash, const instantsend::InstantSendLockPtr& islock, NodeId from)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);

    bool PreVerifyIsLock(const uint256& hash, const instantsend::InstantSendLockPtr& islock, NodeId from) const;

    bool AlreadyHave(const CInv& inv) const EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);
    bool GetInstantSendLockByHash(const uint256& hash, instantsend::InstantSendLock& ret) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);
    instantsend::InstantSendLockPtr GetInstantSendLockByTxid(const uint256& txid) const;

    void TransactionIsRemoved(const CTransactionRef& tx) EXCLUSIVE_LOCKS_REQUIRED(!cs_height_cache);
    void RemoveConflictingLock(const uint256& islockHash, const instantsend::InstantSendLock& islock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_height_cache);
    void TryEmplacePendingLock(const uint256& hash, const NodeId id, const instantsend::InstantSendLockPtr& islock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks);

    size_t GetInstantSendLockCount() const;

    struct Counts {
        size_t m_verified{0};
        size_t m_unverified{0};
        size_t m_awaiting_tx{0};
        size_t m_unprotected_tx{0};
    };
    Counts GetCounts() const EXCLUSIVE_LOCKS_REQUIRED(!cs_pendingLocks, !cs_nonLocked);

    void CacheBlockHeight(const CBlockIndex* const block_index) const EXCLUSIVE_LOCKS_REQUIRED(!cs_height_cache);
    void CacheDisconnectBlock(const CBlockIndex* pindexDisconnected) EXCLUSIVE_LOCKS_REQUIRED(!cs_height_cache);
    std::optional<int> GetCachedHeight(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(!cs_height_cache);
    void CacheTipHeight(const CBlockIndex* const tip) const EXCLUSIVE_LOCKS_REQUIRED(!cs_height_cache);
    int GetTipHeight() const EXCLUSIVE_LOCKS_REQUIRED(!cs_height_cache);

    bool IsInstantSendEnabled() const;
    /**
     * If true, MN should sign all transactions, if false, MN should not sign
     * transactions in mempool, but should sign txes included in a block. This
     * allows ChainLocks to continue even while this spork is disabled.
     */
    bool RejectConflictingBlocks() const;
    Uint256HashMap<instantsend::InstantSendLockPtr> RemoveConfirmedInstantSendLocks(const CBlockIndex* pindex)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_nonLocked, !cs_pendingRetry);
};
} // namespace llmq

#endif // BITCOIN_INSTANTSEND_INSTANTSEND_H
