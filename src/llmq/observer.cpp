// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/observer.h>

#include <llmq/debug.h>
#include <llmq/dkgsessionmgr.h>

namespace llmq {
ObserverContext::ObserverContext(CDeterministicMNManager& dmnman, llmq::CQuorumManager& qman,
                                 llmq::CQuorumSnapshotManager& qsnapman, const ChainstateManager& chainman,
                                 const CSporkManager& sporkman, const util::DbWrapperParams& db_params) :
    QuorumRole{qman},
    dkgdbgman{std::make_unique<llmq::CDKGDebugManager>(dmnman, qsnapman, chainman)},
    qdkgsman{std::make_unique<llmq::CDKGSessionManager>(dmnman, qsnapman, chainman, sporkman, db_params)}
{
}

ObserverContext::~ObserverContext() = default;

void ObserverContext::InitializeCurrentBlockTip(const CBlockIndex* tip, bool ibd)
{
    UpdatedBlockTip(tip, nullptr, ibd);
}

void ObserverContext::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    qdkgsman->UpdatedBlockTip(pindexNew, fInitialDownload);
}
} // namespace llmq
