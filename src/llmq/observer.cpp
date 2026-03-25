// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/observer.h>

#include <llmq/debug.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/quorums.h>
#include <msg_result.h>

#include <chain.h>
#include <validation.h>

namespace llmq {
ObserverContext::ObserverContext(CBLSWorker& bls_worker, CDeterministicMNManager& dmnman,
                                 CMasternodeMetaMan& mn_metaman,
                                 llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumManager& qman,
                                 llmq::CQuorumSnapshotManager& qsnapman, const ChainstateManager& chainman,
                                 const CSporkManager& sporkman, const util::DbWrapperParams& db_params) :
    QuorumRole{qman},
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

void ObserverContext::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    qdkgsman->UpdatedBlockTip(pindexNew, fInitialDownload);
}
} // namespace llmq
