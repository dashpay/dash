// Copyright (c) 2019 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_QUORUMS_INSTANTSEND_H
#define DASH_QUORUMS_INSTANTSEND_H

#include "quorums_signing.h"

#include "coins.h"
#include "unordered_lru_cache.h"
#include "primitives/transaction.h"

#include <unordered_map>
#include <unordered_set>

namespace llmq
{

class CInstantSendLock
{
public:
    std::vector<COutPoint> inputs;
    uint256 txid;
    CBLSLazySignature sig;

public:
    ADD_SERIALIZE_METHODS

    template<typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(inputs);
        READWRITE(txid);
        READWRITE(sig);
    }

    uint256 GetRequestId() const;
};

typedef std::shared_ptr<CInstantSendLock> CInstantSendLockPtr;

class CInstantSendDb
{
private:
    CDBWrapper& db;

    unordered_lru_cache<uint256, CInstantSendLockPtr, StaticSaltedHasher, 10000> islockCache;
    unordered_lru_cache<uint256, uint256, StaticSaltedHasher, 10000> txidCache;
    unordered_lru_cache<COutPoint, uint256, SaltedOutpointHasher, 10000> outpointCache;

public:
    CInstantSendDb(CDBWrapper& _db) : db(_db) {}

    /**
     * This method is called when an InstantSend Lock is processed and adds the lock to the database instantsend.dat
     * @param hash The hash of the InstantSend Lock
     * @param islock The InstantSend Lock object itself
     */
    void WriteNewInstantSendLock(const uint256& hash, const CInstantSendLock& islock);
    /**
     * This method removes a InstantSend Lock from instantsend.dat and is called when a tx with an IS lock is confirmed and Chainlocked
     * @param batch Object used to batch many calls together
     * @param hash The hash of the InstantSend Lock
     * @param islock The InstantSend Lock object itself
     */
    void RemoveInstantSendLock(CDBBatch& batch, const uint256& hash, CInstantSendLockPtr islock);

    /**
     * This method updates a DB entry for an InstantSend Lock from being not included in a block to being included in a block
     * @param hash The hash of the InstantSend Lock
     * @param nHeight The height that the transaction was included at
     */
    void WriteInstantSendLockMined(const uint256& hash, int nHeight);
    /**
     * This method is called when a block gets orphaned for every tx in that block. It marks the ISLocked tx as not included in a block
     * @param hash The hash of the InstantSend Lock
     * @param nHeight The height of the block being orphaned
     */
    void RemoveInstantSendLockMined(const uint256& hash, int nHeight);
    /**
     * Marks an InstantSend Lock as archived.
     * @param batch Object used to batch many calls together
     * @param hash The hash of the InstantSend Lock
     * @param nHeight The height that the transaction was included at
     */
    void WriteInstantSendLockArchived(CDBBatch& batch, const uint256& hash, int nHeight);
    /**
     * Archives and deletes all IS Locks which were mined into a block before nUntilHeight
     * @param nUntilHeight Removes all IS Locks confirmed up until nUntilHeight
     * @return returns an unordered_map of the hash of the IS Locks and a pointer object to the IS Locks for all IS Locks which were removed
     */
    std::unordered_map<uint256, CInstantSendLockPtr> RemoveConfirmedInstantSendLocks(int nUntilHeight);
    /**
     * Removes IS Locks from the archive if the tx was confirmed 100 blocks before nUntilHeight
     * @param nUntilHeight the height from which to base the remove of archive IS Locks
     */
    void RemoveArchivedInstantSendLocks(int nUntilHeight);
    /**
     * Checks if the archive contains a certain IS Lock Hash
     * @param islockHash The hash to check for
     * @return Returns true if that IS Lock hash is present in the archive, otherwise returns false
     */
    bool HasArchivedInstantSendLock(const uint256& islockHash);
    /**
     * Gets the number of IS Locks which have not been confirmed by a block
     * @return size_t value of the number of IS Locks not confirmed by a block
     */
    size_t GetInstantSendLockCount();

    /**
     * Gets a pointer to the IS Lock based on the hash
     * @param hash The hash of the IS Lock
     * @return A Pointer object to the IS Lock, returns nullptr if it doesn't exsist
     */
    CInstantSendLockPtr GetInstantSendLockByHash(const uint256& hash);
    /**
     * Gets an IS Lock hash based on the txid the IS Lock is for
     * @param txid The txid which is being searched for
     * @return Returns the hash the IS Lock of the specified txid, returns uint256() if it doesn't exsist
     */
    uint256 GetInstantSendLockHashByTxid(const uint256& txid);
    /**
     * Gets an IS Lock pointer object from the txid given
     * @param txid The txid for which the IS Lock Pointer is being returned
     * @return Returns the IS Lock Pointer assosiated with the txid, returns nullptr if it doesn't exsist
     */
    CInstantSendLockPtr GetInstantSendLockByTxid(const uint256& txid);
    /**
     *
     * @param outpoint
     * @return
     */
    CInstantSendLockPtr GetInstantSendLockByInput(const COutPoint& outpoint);

    std::vector<uint256> GetInstantSendLocksByParent(const uint256& parent);
    std::vector<uint256> RemoveChainedInstantSendLocks(const uint256& islockHash, const uint256& txid, int nHeight);
};

class CInstantSendManager : public CRecoveredSigsListener
{
private:
    CCriticalSection cs;
    CInstantSendDb db;

    std::thread workThread;
    CThreadInterrupt workInterrupt;

    /**
     * Request ids of inputs that we signed. Used to determine if a recovered signature belongs to an
     * in-progress input lock.
     */
    std::unordered_set<uint256, StaticSaltedHasher> inputRequestIds;

    /**
     * These are the islocks that are currently in the middle of being created. Entries are created when we observed
     * recovered signatures for all inputs of a TX. At the same time, we initiate signing of our sigshare for the islock.
     * When the recovered sig for the islock later arrives, we can finish the islock and propagate it.
     */
    std::unordered_map<uint256, CInstantSendLock, StaticSaltedHasher> creatingInstantSendLocks;
    // maps from txid to the in-progress islock
    std::unordered_map<uint256, CInstantSendLock*, StaticSaltedHasher> txToCreatingInstantSendLocks;

    // Incoming and not verified yet
    std::unordered_map<uint256, std::pair<NodeId, CInstantSendLock>> pendingInstantSendLocks;

    // TXs which are neither IS locked nor ChainLocked. We use this to determine for which TXs we need to retry IS locking
    // of child TXs
    struct NonLockedTxInfo {
        const CBlockIndex* pindexMined{nullptr};
        CTransactionRef tx;
        std::unordered_set<uint256, StaticSaltedHasher> children;
    };
    std::unordered_map<uint256, NonLockedTxInfo, StaticSaltedHasher> nonLockedTxs;
    std::unordered_multimap<uint256, std::pair<uint32_t, uint256>> nonLockedTxsByInputs;

    std::unordered_set<uint256, StaticSaltedHasher> pendingRetryTxs;

public:
    CInstantSendManager(CDBWrapper& _llmqDb);
    ~CInstantSendManager();

    void Start();
    void Stop();
    void InterruptWorkerThread();

public:
    bool ProcessTx(const CTransaction& tx, const Consensus::Params& params);
    bool CheckCanLock(const CTransaction& tx, bool printDebug, const Consensus::Params& params);
    bool CheckCanLock(const COutPoint& outpoint, bool printDebug, const uint256& txHash, CAmount* retValue, const Consensus::Params& params);
    bool IsLocked(const uint256& txHash);
    bool IsConflicted(const CTransaction& tx);
    CInstantSendLockPtr GetConflictingLock(const CTransaction& tx);

    virtual void HandleNewRecoveredSig(const CRecoveredSig& recoveredSig);
    void HandleNewInputLockRecoveredSig(const CRecoveredSig& recoveredSig, const uint256& txid);
    void HandleNewInstantSendLockRecoveredSig(const CRecoveredSig& recoveredSig);

    void TrySignInstantSendLock(const CTransaction& tx);

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman);
    void ProcessMessageInstantSendLock(CNode* pfrom, const CInstantSendLock& islock, CConnman& connman);
    bool PreVerifyInstantSendLock(NodeId nodeId, const CInstantSendLock& islock, bool& retBan);
    bool ProcessPendingInstantSendLocks();
    std::unordered_set<uint256> ProcessPendingInstantSendLocks(int signHeight, const std::unordered_map<uint256, std::pair<NodeId, CInstantSendLock>>& pend, bool ban);
    void ProcessInstantSendLock(NodeId from, const uint256& hash, const CInstantSendLock& islock);
    void UpdateWalletTransaction(const CTransactionRef& tx, const CInstantSendLock& islock);

    void ProcessNewTransaction(const CTransactionRef& tx, const CBlockIndex* pindex);
    void TransactionAddedToMempool(const CTransactionRef& tx);
    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex, const std::vector<CTransactionRef>& vtxConflicted);
    void BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexDisconnected);

    void AddNonLockedTx(const CTransactionRef& tx, const CBlockIndex* pindexMined);
    void RemoveNonLockedTx(const uint256& txid, bool retryChildren);
    void RemoveConflictedTx(const CTransaction& tx);

    void NotifyChainLock(const CBlockIndex* pindexChainLock);
    void UpdatedBlockTip(const CBlockIndex* pindexNew);

    void HandleFullyConfirmedBlock(const CBlockIndex* pindex);

    void RemoveMempoolConflictsForLock(const uint256& hash, const CInstantSendLock& islock);
    void ResolveBlockConflicts(const uint256& islockHash, const CInstantSendLock& islock);
    void RemoveChainLockConflictingLock(const uint256& islockHash, const CInstantSendLock& islock);
    void AskNodesForLockedTx(const uint256& txid);
    bool ProcessPendingRetryLockTxs();

    bool AlreadyHave(const CInv& inv);
    bool GetInstantSendLockByHash(const uint256& hash, CInstantSendLock& ret);

    size_t GetInstantSendLockCount();

    void WorkThreadMain();
};

extern CInstantSendManager* quorumInstantSendManager;

bool IsInstantSendEnabled();

} // namespace llmq

#endif//DASH_QUORUMS_INSTANTSEND_H
