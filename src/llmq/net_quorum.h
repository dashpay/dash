// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_NET_QUORUM_H
#define BITCOIN_LLMQ_NET_QUORUM_H

#include <llmq/options.h>
#include <llmq/quorums.h>
#include <net_processing.h>
#include <sync.h>
#include <unordered_lru_cache.h>
#include <util/threadinterrupt.h>
#include <validationinterface.h>

#include <ctpl_stl.h>
#include <gsl/pointers.h>

#include <map>

class CActiveMasternodeManager;
class CBlockIndex;
class CBLSWorker;
class CConnman;
class CDeterministicMNManager;
class CMasternodeSync;
class CSporkManager;
namespace Consensus {
struct LLMQParams;
} // namespace Consensus
namespace llmq {
class CQuorumManager;
class CQuorumSnapshotManager;
class QuorumRole;
} // namespace llmq

namespace llmq {
/**
 * NetHandler responsible for all quorum networking:
 *  - QGETDATA / QDATA message processing (quorum vvec and encrypted contribution exchange)
 *  - Background quorum peer connection management (CheckQuorumConnections)
 *  - Background vvec and sk-share data recovery threads
 *  - Periodic cleanup of old quorum DB entries
 *
 * Owned exclusively by PeerManagerImpl via AddExtraHandler. No other subsystem
 * holds a reference to this object.
 */
class NetQuorum final : public NetHandler, public CValidationInterface
{
public:
    NetQuorum(PeerManagerInternal* peer_manager, CBLSWorker& bls_worker,
              CConnman& connman, CDeterministicMNManager& dmnman, CQuorumManager& qman,
              CQuorumSnapshotManager& qsnapman, const ChainstateManager& chainman,
              const CMasternodeSync& mn_sync, const CSporkManager& sporkman,
              QuorumRole* quorum_role, CActiveMasternodeManager* nodeman,
              int16_t worker_count, const QvvecSyncModeMap& sync_map, bool quorums_recovery);

    // NetHandler
    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;
    void Start() override;
    void Stop() override;
    void Interrupt() override { quorumThreadInterrupt(); }

protected:
    // CValidationInterface
    void InitializeCurrentBlockTip(const CBlockIndex* tip, bool ibd) override;
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork,
                         bool fInitialDownload) override;

private:
    DataRequestStatus RequestQuorumData(CNode& peer, const CQuorum& quorum, uint16_t nDataMask,
                                        const uint256& proTxHash = uint256()) const;

    Uint256HashSet GetQuorumsToDelete(const Consensus::LLMQParams& llmqParams,
                                      gsl::not_null<const CBlockIndex*> pindexNew) const;
    void CheckQuorumConnections(const Consensus::LLMQParams& llmqParams,
                                         gsl::not_null<const CBlockIndex*> pindexNew) const;
    void TriggerQuorumDataRecoveryThreads(gsl::not_null<const CBlockIndex*> block_index) const;
    void DataRecoveryThread(gsl::not_null<const CBlockIndex*> block_index, CQuorumCPtr pQuorum,
                            uint16_t data_mask, const uint256& protx_hash, size_t start_offset) const;
    void StartVvecSyncThread(gsl::not_null<const CBlockIndex*> block_index, CQuorumCPtr pQuorum) const;
    void TryStartVvecSyncThread(gsl::not_null<const CBlockIndex*> block_index, CQuorumCPtr pQuorum,
                                bool fWeAreQuorumTypeMember) const;
    void StartSkShareRecoveryThread(gsl::not_null<const CBlockIndex*> pIndex, CQuorumCPtr pQuorum,
                                    uint16_t nDataMaskIn) const;
    /// Returns the start offset for the masternode with the given proTxHash. This offset is applied when picking data
    /// recovery members of a quorum's memberlist and is calculated based on a list of all member of all active quorums
    /// for the given llmqType in a way that each member should receive the same number of request if all active
    /// llmqType members requests data from one llmqType quorum.
    size_t GetQuorumRecoveryStartOffset(const CQuorum& quorum,
                                        gsl::not_null<const CBlockIndex*> pIndex) const;
    void StartCleanupOldQuorumDataThread(gsl::not_null<const CBlockIndex*> pIndex) const;

    bool ProcessContribQGETDATA(bool request_limit_exceeded, CDataStream& ssResponseData,
                                const CQuorum& quorum, CQuorumDataRequest& request,
                                gsl::not_null<const CBlockIndex*> block_index) const;
    bool ProcessContribQDATA(CNode& pfrom, CDataStream& vRecv,
                             CQuorum& quorum, CQuorumDataRequest& request);

private:
    CBLSWorker& m_bls_worker;
    CConnman& m_connman;
    CDeterministicMNManager& m_dmnman;
    CQuorumManager& m_qman;
    CQuorumSnapshotManager& m_qsnapman;
    const ChainstateManager& m_chainman;
    const CMasternodeSync& m_mn_sync;
    const CSporkManager& m_sporkman;
    //! Non-null only when masternode or observer mode is active
    QuorumRole* const m_role;
    //! Non-null only in masternode mode
    CActiveMasternodeManager* const m_nodeman;

    const int16_t m_worker_count;
    const QvvecSyncModeMap m_sync_map;
    const bool m_quorums_recovery;

    mutable Mutex cs_cleanup;
    mutable std::map<Consensus::LLMQType, Uint256LruHashMap<uint256>> cleanupQuorumsCache
        GUARDED_BY(cs_cleanup);

    mutable ctpl::thread_pool workerPool;
    mutable CThreadInterrupt quorumThreadInterrupt;
};
} // namespace llmq

#endif // BITCOIN_LLMQ_NET_QUORUM_H
