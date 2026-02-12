// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INSTANTSEND_NET_INSTANTSEND_H
#define BITCOIN_INSTANTSEND_NET_INSTANTSEND_H

#include <net_processing.h>
#include <util/threadinterrupt.h>
#include <validationinterface.h>

#include <memory>
#include <optional>
#include <thread>
#include <vector>

class CChainState;

namespace Consensus {
struct LLMQParams;
} // namespace Consensus

class CMasternodeSync;
class CTxMemPool;

namespace instantsend {
struct InstantSendLock;
using InstantSendLockPtr = std::shared_ptr<InstantSendLock>;
struct PendingISLockEntry;
class InstantSendSigner;
} // namespace instantsend
namespace llmq {
class CInstantSendManager;
class CQuorumManager;
} // namespace llmq

class NetInstantSend final : public NetHandler, public CValidationInterface
{
public:
    NetInstantSend(PeerManagerInternal* peer_manager, llmq::CInstantSendManager& is_manager,
                   instantsend::InstantSendSigner* signer, llmq::CQuorumManager& qman, CChainState& chainstate,
                   CTxMemPool& mempool, const CMasternodeSync& mn_sync) :
        NetHandler(peer_manager),
        m_is_manager{is_manager},
        m_signer{signer},
        m_qman(qman),
        m_chainstate{chainstate},
        m_mempool{mempool},
        m_mn_sync{mn_sync}
    {
        workInterrupt.reset();
    }
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;

    void Start() override;
    void Stop() override;
    void Interrupt() override { workInterrupt(); };

    void WorkThreadMain();

protected:
    // -- CValidationInterface
    void SynchronousUpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork,
                                    bool fInitialDownload) override;
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override;
    void TransactionAddedToMempool(const CTransactionRef&, int64_t, uint64_t mempool_sequence) override;
    void TransactionRemovedFromMempool(const CTransactionRef& ptx, MemPoolRemovalReason reason,
                                       uint64_t mempool_sequence) override;
    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex) override;
    void BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexDisconnected) override;
    void NotifyChainLock(const CBlockIndex* pindex, const std::shared_ptr<const chainlock::ChainLockSig>& clsig) override;

private:
    struct BatchVerificationData;

    bool ValidateIncomingISLock(const instantsend::InstantSendLock& islock, NodeId node_id);
    std::optional<int> ResolveCycleHeight(const uint256& cycle_hash);
    bool ValidateDeterministicCycleHeight(int cycle_height, const Consensus::LLMQParams& llmq_params, NodeId node_id);

    std::unique_ptr<BatchVerificationData> BuildVerificationBatch(
        const Consensus::LLMQParams& llmq_params, int signOffset,
        const std::vector<instantsend::PendingISLockEntry>& pend);
    Uint256HashSet ApplyVerificationResults(
        const Consensus::LLMQParams& llmq_params, bool ban,
        BatchVerificationData& data,
        const std::vector<instantsend::PendingISLockEntry>& pend);

    void ProcessPendingISLocks(std::vector<instantsend::PendingISLockEntry>&& locks_to_process);
    void ProcessInstantSendLock(NodeId from, const uint256& hash, const instantsend::InstantSendLockPtr& islock);
    void RemoveMempoolConflictsForLock(const uint256& hash, const instantsend::InstantSendLock& islock);

    Uint256HashSet ProcessPendingInstantSendLocks(
        const Consensus::LLMQParams& llmq_params, int signOffset, bool ban,
        const std::vector<instantsend::PendingISLockEntry>& pend);

    void ResolveBlockConflicts(const uint256& islockHash, const instantsend::InstantSendLock& islock);

    void TruncateRecoveredSigsForInputs(const instantsend::InstantSendLock& islock);

    void HandleFullyConfirmedBlock(const CBlockIndex* pindex);
    void ClearConflicting(const Uint256HashMap<CTransactionRef>& to_delete);

    llmq::CInstantSendManager& m_is_manager;
    instantsend::InstantSendSigner* m_signer; // non-null only for masternode
    llmq::CQuorumManager& m_qman;
    CChainState& m_chainstate;
    CTxMemPool& m_mempool;
    const CMasternodeSync& m_mn_sync;

    std::thread workThread;
    CThreadInterrupt workInterrupt;
};

#endif // BITCOIN_INSTANTSEND_NET_INSTANTSEND_H
