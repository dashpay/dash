// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/context.h>

#include <active/dkgsessionhandler.h>
#include <active/masternode.h>
#include <bls/bls_worker.h>
#include <chainlock/handler.h>
#include <chainlock/signing.h>
#include <evo/deterministicmns.h>
#include <governance/governance.h>
#include <governance/signing.h>
#include <governance/superblock.h>
#include <instantsend/instantsend.h>
#include <instantsend/signing.h>
#include <llmq/debug.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/ehf_signals.h>
#include <llmq/quorums.h>
#include <llmq/quorumsman.h>
#include <llmq/signing_shares.h>
#include <masternode/sync.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>

ActiveContext::ActiveContext(CBLSWorker& bls_worker, ChainstateManager& chainman, CConnman& connman,
                             CDeterministicMNManager& dmnman, CGovernanceManager& govman,
                             governance::SuperblockManager& superblocks, CMasternodeMetaMan& mn_metaman,
                             CSporkManager& sporkman, const chainlock::Chainlocks& chainlocks, CTxMemPool& mempool,
                             chainlock::ChainlockHandler& clhandler, llmq::CInstantSendManager& isman,
                             llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumManager& qman,
                             llmq::CQuorumSnapshotManager& qsnapman, llmq::CSigningManager& sigman,
                             const CMasternodeSync& mn_sync, const CBLSSecretKey& operator_sk,
                             const util::DbWrapperParams& db_params, bool quorums_watch) :
    llmq::QuorumRole{qman},
    m_bls_worker{bls_worker},
    m_quorums_watch{quorums_watch},
    nodeman{std::make_unique<CActiveMasternodeManager>(connman, dmnman, operator_sk)},
    dkgdbgman{std::make_unique<llmq::CDKGDebugManager>(dmnman, qsnapman, chainman)},
    qdkgsman{std::make_unique<llmq::CDKGSessionManager>(dmnman, qsnapman, chainman, sporkman, db_params, quorums_watch)},
    shareman{std::make_unique<llmq::CSigSharesManager>(connman, chainman, sigman, *nodeman, qman, sporkman)},
    gov_signer{std::make_unique<GovernanceSigner>(connman, dmnman, govman, superblocks, *nodeman, chainman, mn_sync)},
    ehf_sighandler{std::make_unique<llmq::CEHFSignalsHandler>(chainman, sigman, *shareman, qman)},
    cl_signer{std::make_unique<chainlock::ChainLockSigner>(chainman.ActiveChainstate(), chainlocks, clhandler, isman,
                                                           qman, sigman, *shareman, mn_sync)},
    is_signer{std::make_unique<instantsend::InstantSendSigner>(chainman.ActiveChainstate(), chainlocks, isman, sigman,
                                                               *shareman, qman, sporkman, mempool, mn_sync)}
{
    qdkgsman->InitializeHandlers([&](const Consensus::LLMQParams& llmq_params,
                                     int quorum_idx) -> std::unique_ptr<llmq::ActiveDKGSessionHandler> {
        return std::make_unique<llmq::ActiveDKGSessionHandler>(bls_worker, dmnman, mn_metaman, *dkgdbgman, *qdkgsman,
                                                               qblockman, qsnapman, *nodeman, chainman, sporkman,
                                                               llmq_params, quorums_watch, quorum_idx);
    });
    m_qman.ConnectManagers(this, qdkgsman.get());
}

ActiveContext::~ActiveContext()
{
    m_qman.DisconnectManagers();
}

void ActiveContext::Start()
{
    cl_signer->Start();
    cl_signer->RegisterRecoveryInterface();
    is_signer->RegisterRecoveryInterface();
    shareman->RegisterRecoveryInterface();

    RegisterValidationInterface(cl_signer.get());
}

void ActiveContext::Stop()
{
    UnregisterValidationInterface(cl_signer.get());

    shareman->UnregisterRecoveryInterface();
    is_signer->UnregisterRecoveryInterface();
    cl_signer->UnregisterRecoveryInterface();
    cl_signer->Stop();
}

CCoinJoinServer& ActiveContext::GetCJServer() const
{
    return *Assert(m_cj_server);
}

void ActiveContext::SetCJServer(gsl::not_null<CCoinJoinServer*> cj_server)
{
    // Prohibit double initialization
    assert(m_cj_server == nullptr);
    m_cj_server = cj_server;
}

void ActiveContext::InitializeCurrentBlockTip(const CBlockIndex* tip, bool ibd)
{
    UpdatedBlockTip(tip, nullptr, ibd);
}

void ActiveContext::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    nodeman->UpdatedBlockTip(pindexNew, pindexFork, fInitialDownload);
    ehf_sighandler->UpdatedBlockTip(pindexNew);
    gov_signer->UpdatedBlockTip(pindexNew);
    qdkgsman->UpdatedBlockTip(pindexNew, fInitialDownload);
}

bool ActiveContext::IsMasternode() const
{
    // We are only initialized if masternode mode is enabled
    return true;
}

bool ActiveContext::IsWatching() const
{
    // Watch-only mode can co-exist with masternode mode
    return m_quorums_watch;
}

uint256 ActiveContext::GetProTxHash() const
{
    return nodeman->GetProTxHash();
}

bool ActiveContext::SetQuorumSecretKeyShare(llmq::CQuorum& quorum, Span<CBLSSecretKey> skContributions) const
{
    return quorum.SetSecretKeyShare(m_bls_worker.AggregateSecretKeys(skContributions), nodeman->GetProTxHash());
}
