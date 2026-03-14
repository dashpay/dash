// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ACTIVE_CONTEXT_H
#define BITCOIN_ACTIVE_CONTEXT_H

#include <llmq/quorumsman.h>

#include <validationinterface.h>

#include <gsl/pointers.h>
#include <span.h>

#include <memory>

class CActiveMasternodeManager;
class CBLSWorker;
class CCoinJoinServer;
class CGovernanceManager;
class CMasternodeMetaMan;
class CMNHFManager;
class CTxMemPool;
class GovernanceSigner;
class PeerManager;
namespace chainlock {
class Chainlocks;
class ChainlockHandler;
class ChainLockSigner;
} // namespace chainlock
namespace instantsend {
class InstantSendSigner;
} // namespace instantsend
namespace llmq {
class CDKGDebugManager;
class CEHFSignalsHandler;
class CInstantSendManager;
class CSigningManager;
class CSigSharesManager;
} // namespace llmq
namespace util {
struct DbWrapperParams;
} // namespace util

struct ActiveContext final : public llmq::QuorumRole, public CValidationInterface {
private:
    CBLSWorker& m_bls_worker;
    const bool m_quorums_watch{false};

public:
    ActiveContext() = delete;
    ActiveContext(const ActiveContext&) = delete;
    ActiveContext& operator=(const ActiveContext&) = delete;
    explicit ActiveContext(CBLSWorker& bls_worker, ChainstateManager& chainman, CConnman& connman,
                           CDeterministicMNManager& dmnman, CGovernanceManager& govman, CMasternodeMetaMan& mn_metaman,
                           CSporkManager& sporkman, const chainlock::Chainlocks& chainlocks, CTxMemPool& mempool,
                           chainlock::ChainlockHandler& clhandler, llmq::CInstantSendManager& isman,
                           llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumManager& qman,
                           llmq::CQuorumSnapshotManager& qsnapman, llmq::CSigningManager& sigman,
                           const CMasternodeSync& mn_sync, const CBLSSecretKey& operator_sk,
                           const llmq::QvvecSyncModeMap& sync_map, const util::DbWrapperParams& db_params,
                           bool quorums_recovery, bool quorums_watch);
    ~ActiveContext();

    void Start(CConnman& connman, PeerManager& peerman, int16_t worker_count);
    void Stop();
    void InitializeCurrentBlockTip(const CBlockIndex* tip, bool ibd);

    CCoinJoinServer& GetCJServer() const;
    void SetCJServer(gsl::not_null<CCoinJoinServer*> cj_server);

    // QuorumRole
    bool IsMasternode() const override;
    bool IsWatching() const override;
    bool SetQuorumSecretKeyShare(llmq::CQuorum& quorum, Span<CBLSSecretKey> skContributions) const override;
    [[nodiscard]] MessageProcessingResult ProcessContribQGETDATA(bool request_limit_exceeded, CDataStream& vStream,
                                                                 const llmq::CQuorum& quorum,
                                                                 llmq::CQuorumDataRequest& request,
                                                                 gsl::not_null<const CBlockIndex*> block_index) override;
    [[nodiscard]] MessageProcessingResult ProcessContribQDATA(CNode& pfrom, CDataStream& vStream,
                                                              llmq::CQuorum& quorum,
                                                              llmq::CQuorumDataRequest& request) override;

protected:
    void CheckQuorumConnections(const Consensus::LLMQParams& llmqParams,
                                gsl::not_null<const CBlockIndex*> pindexNew) const override;
    void TriggerQuorumDataRecoveryThreads(gsl::not_null<const CBlockIndex*> block_index) const override;

    // CValidationInterface
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override;

public:
    /*
     * Entities that are only utilized when masternode mode is enabled
     * and are accessible in their own right
     */
    const std::unique_ptr<CActiveMasternodeManager> nodeman;
    const std::unique_ptr<llmq::CDKGDebugManager> dkgdbgman;
    const std::unique_ptr<llmq::CDKGSessionManager> qdkgsman;
    const std::unique_ptr<llmq::CSigSharesManager> shareman;

private:
    const std::unique_ptr<GovernanceSigner> gov_signer;
    const std::unique_ptr<llmq::CEHFSignalsHandler> ehf_sighandler;
    const std::unique_ptr<chainlock::ChainLockSigner> cl_signer;

public:
    const std::unique_ptr<instantsend::InstantSendSigner> is_signer;

    /** Owned by PeerManager, use GetCJServer() */
    CCoinJoinServer* m_cj_server{nullptr};

private:
    /// Returns the start offset for the masternode with the given proTxHash. This offset is applied when picking data
    /// recovery members of a quorum's memberlist and is calculated based on a list of all member of all active quorums
    /// for the given llmqType in a way that each member should receive the same number of request if all active
    /// llmqType members requests data from one llmqType quorum.
    size_t GetQuorumRecoveryStartOffset(const llmq::CQuorum& quorum,
                                        gsl::not_null<const CBlockIndex*> pIndex) const;
    void StartDataRecoveryThread(gsl::not_null<const CBlockIndex*> pIndex, llmq::CQuorumCPtr pQuorum,
                                 uint16_t nDataMaskIn) const;
};

#endif // BITCOIN_ACTIVE_CONTEXT_H
