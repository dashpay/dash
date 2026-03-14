// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/observer/context.h>

#include <llmq/debug.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/quorums.h>
#include <msg_result.h>

#include <chain.h>
#include <net.h>
#include <validation.h>

namespace llmq {
ObserverContext::ObserverContext(CBLSWorker& bls_worker, CConnman& connman, CDeterministicMNManager& dmnman,
                                 CMasternodeMetaMan& mn_metaman, CMasternodeSync& mn_sync,
                                 llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumManager& qman,
                                 llmq::CQuorumSnapshotManager& qsnapman, const ChainstateManager& chainman,
                                 const CSporkManager& sporkman, const llmq::QvvecSyncModeMap& sync_map,
                                 const util::DbWrapperParams& db_params, bool quorums_recovery) :
    QuorumRole{connman, dmnman, qman, qsnapman, chainman, mn_sync, sporkman, sync_map, quorums_recovery},
    dkgdbgman{std::make_unique<llmq::CDKGDebugManager>(dmnman, qsnapman, chainman)},
    qdkgsman{std::make_unique<llmq::CDKGSessionManager>(dmnman, qsnapman, chainman, sporkman, db_params,
                                                        /*quorums_watch=*/true)}
{
    qdkgsman->InitializeHandlers([&](const Consensus::LLMQParams& llmq_params,
                                     [[maybe_unused]] int quorum_idx) -> std::unique_ptr<llmq::CDKGSessionHandler> {
        return std::make_unique<llmq::CDKGSessionHandler>(llmq_params);
    });
    m_qman.ConnectManagers(this, qdkgsman.get());
}

ObserverContext::~ObserverContext()
{
    m_qman.DisconnectManagers();
}

MessageProcessingResult ObserverContext::ProcessContribQGETDATA(bool request_limit_exceeded, CDataStream& vStream,
                                                                const CQuorum& quorum, CQuorumDataRequest& request,
                                                                gsl::not_null<const CBlockIndex*> block_index)
{
    // Watch-only nodes cannot provide encrypted contributions
    if (request.GetDataMask() & CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS) {
        request.SetError(CQuorumDataRequest::Errors::ENCRYPTED_CONTRIBUTIONS_MISSING);
        return request_limit_exceeded ? MisbehavingError{25, "request limit exceeded"} : MessageProcessingResult{};
    }
    return {};
}

MessageProcessingResult ObserverContext::ProcessContribQDATA(CNode& pfrom, CDataStream& vStream, CQuorum& quorum,
                                                             CQuorumDataRequest& request)
{
    // Watch-only nodes ignore encrypted contributions
    return {};
}

void ObserverContext::InitializeCurrentBlockTip(const CBlockIndex* tip, bool ibd)
{
    UpdatedBlockTip(tip, nullptr, ibd);
    if (tip) {
        qman_handler->InitializeQuorumConnections(tip);
    }
}

void ObserverContext::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    qdkgsman->UpdatedBlockTip(pindexNew, fInitialDownload);
    QuorumRole::UpdatedBlockTip(pindexNew, fInitialDownload);
}
} // namespace llmq
