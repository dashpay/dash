// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INSTANTSEND_SIGNING_H
#define BITCOIN_INSTANTSEND_SIGNING_H

#include <instantsend/lock.h>
#include <llmq/signing.h>

class CMasternodeSync;
class CSporkManager;
class CTxMemPool;
struct MessageProcessingResult;

namespace Consensus {
struct Params;
} // namespace Consensus
namespace llmq {
class CChainLocksHandler;
class CSigningManager;
class CSigSharesManager;
class CQuorumManager;
} // namespace llmq

namespace instantsend {

class InstantSendStorage
{
public:
    virtual ~InstantSendStorage() = default;

    virtual bool IsInstantSendEnabled() const = 0;
    virtual bool IsLocked(const uint256& txHash) const = 0;
    virtual instantsend::InstantSendLockPtr GetConflictingLock(const CTransaction& tx) const = 0;
    virtual void TryEmplacePendingLock(const uint256& hash, const NodeId id,
                                       const instantsend::InstantSendLockPtr& islock) = 0;
};

class InstantSendSigner : public llmq::CRecoveredSigsListener
{
private:
    CChainState& m_chainstate;
    llmq::CChainLocksHandler& m_clhandler;
    InstantSendStorage& m_isman;
    llmq::CSigningManager& m_sigman;
    llmq::CSigSharesManager& m_shareman;
    llmq::CQuorumManager& m_qman;
    CSporkManager& m_sporkman;
    CTxMemPool& m_mempool;
    const CMasternodeSync& m_mn_sync;

private:
    mutable Mutex cs_inputReqests;
    mutable Mutex cs_creating;

    /**
     * Request ids of inputs that we signed. Used to determine if a recovered signature belongs to an
     * in-progress input lock.
     */
    std::unordered_set<uint256, StaticSaltedHasher> inputRequestIds GUARDED_BY(cs_inputReqests);

    /**
     * These are the islocks that are currently in the middle of being created. Entries are created when we observed
     * recovered signatures for all inputs of a TX. At the same time, we initiate signing of our sigshare for the islock.
     * When the recovered sig for the islock later arrives, we can finish the islock and propagate it.
     */
    std::unordered_map<uint256, InstantSendLock, StaticSaltedHasher> creatingInstantSendLocks GUARDED_BY(cs_creating);
    // maps from txid to the in-progress islock
    std::unordered_map<uint256, InstantSendLock*, StaticSaltedHasher> txToCreatingInstantSendLocks GUARDED_BY(cs_creating);

public:
    explicit InstantSendSigner(CChainState& chainstate, llmq::CChainLocksHandler& clhandler, InstantSendStorage& isman,
                               llmq::CSigningManager& sigman, llmq::CSigSharesManager& shareman, llmq::CQuorumManager& qman,
                               CSporkManager& sporkman, CTxMemPool& mempool, const CMasternodeSync& mn_sync);
    ~InstantSendSigner();

    void Start();
    void Stop();

    void ClearInputsFromQueue(const std::unordered_set<uint256, StaticSaltedHasher>& ids)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests);

    void ClearLockFromQueue(const InstantSendLockPtr& islock)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating);

    [[nodiscard]] MessageProcessingResult HandleNewRecoveredSig(const llmq::CRecoveredSig& recoveredSig) override
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests);

    void ProcessPendingRetryLockTxs(const std::vector<CTransactionRef>& retryTxs)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests);
    void ProcessTx(const CTransaction& tx, bool fRetroactive, const Consensus::Params& params)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating, !cs_inputReqests);

private:
    [[nodiscard]] bool CheckCanLock(const CTransaction& tx, bool printDebug, const Consensus::Params& params) const;
    [[nodiscard]] bool CheckCanLock(const COutPoint& outpoint, bool printDebug, const uint256& txHash,
                                    const Consensus::Params& params) const;

    void HandleNewInputLockRecoveredSig(const llmq::CRecoveredSig& recoveredSig, const uint256& txid)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating);
    void HandleNewInstantSendLockRecoveredSig(const llmq::CRecoveredSig& recoveredSig)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating);

    [[nodiscard]] bool IsInstantSendMempoolSigningEnabled() const;

    [[nodiscard]] bool TrySignInputLocks(const CTransaction& tx, bool allowResigning, Consensus::LLMQType llmqType,
                                         const Consensus::Params& params)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_inputReqests);
    void TrySignInstantSendLock(const CTransaction& tx)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_creating);
};
} // namespace instantsend

#endif // BITCOIN_INSTANTSEND_SIGNING_H
