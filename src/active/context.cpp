// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/context.h>

#include <active/dkgsessionhandler.h>
#include <active/masternode.h>
#include <bls/bls_ies.h>
#include <bls/bls_worker.h>
#include <chain.h>
#include <chainlock/handler.h>
#include <chainlock/signing.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <governance/governance.h>
#include <governance/signing.h>
#include <instantsend/instantsend.h>
#include <instantsend/signing.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/debug.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/ehf_signals.h>
#include <llmq/options.h>
#include <llmq/quorums.h>
#include <llmq/quorumsman.h>
#include <llmq/signing_shares.h>
#include <llmq/utils.h>
#include <logging.h>
#include <masternode/sync.h>
#include <msg_result.h>
#include <net.h>
#include <netmessagemaker.h>
#include <util/check.h>
#include <util/helpers.h>
#include <validation.h>
#include <validationinterface.h>

#include <ranges>

ActiveContext::ActiveContext(CBLSWorker& bls_worker, ChainstateManager& chainman, CConnman& connman,
                             CDeterministicMNManager& dmnman, CGovernanceManager& govman, CMasternodeMetaMan& mn_metaman,
                             CSporkManager& sporkman, const chainlock::Chainlocks& chainlocks, CTxMemPool& mempool,
                             chainlock::ChainlockHandler& clhandler, llmq::CInstantSendManager& isman,
                             llmq::CQuorumBlockProcessor& qblockman, llmq::CQuorumManager& qman,
                             llmq::CQuorumSnapshotManager& qsnapman, llmq::CSigningManager& sigman,
                             const CMasternodeSync& mn_sync, const CBLSSecretKey& operator_sk,
                             const llmq::QvvecSyncModeMap& sync_map, const util::DbWrapperParams& db_params,
                             bool quorums_recovery, bool quorums_watch) :
    llmq::QuorumRole{connman, dmnman, qman, qsnapman, chainman, mn_sync, sporkman, sync_map, quorums_recovery},
    m_bls_worker{bls_worker},
    m_quorums_watch{quorums_watch},
    nodeman{std::make_unique<CActiveMasternodeManager>(connman, dmnman, operator_sk)},
    dkgdbgman{std::make_unique<llmq::CDKGDebugManager>(dmnman, qsnapman, chainman)},
    qdkgsman{std::make_unique<llmq::CDKGSessionManager>(dmnman, qsnapman, chainman, sporkman, db_params, quorums_watch)},
    shareman{std::make_unique<llmq::CSigSharesManager>(connman, chainman, sigman, *nodeman, qman, sporkman)},
    gov_signer{std::make_unique<GovernanceSigner>(connman, dmnman, govman, *nodeman, chainman, mn_sync)},
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

void ActiveContext::Start(CConnman& connman, PeerManager& peerman, int16_t worker_count)
{
    llmq::QuorumRole::Start(worker_count);
    qdkgsman->StartThreads(connman, peerman);
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
    qdkgsman->StopThreads();
    llmq::QuorumRole::Stop();
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
    if (tip) {
        llmq::QuorumRole::InitializeQuorumConnections(tip);
    }
}

void ActiveContext::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    nodeman->UpdatedBlockTip(pindexNew, pindexFork, fInitialDownload);
    ehf_sighandler->UpdatedBlockTip(pindexNew);
    gov_signer->UpdatedBlockTip(pindexNew);
    qdkgsman->UpdatedBlockTip(pindexNew, fInitialDownload);
    llmq::QuorumRole::UpdatedBlockTip(pindexNew, fInitialDownload);
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

bool ActiveContext::SetQuorumSecretKeyShare(llmq::CQuorum& quorum, Span<CBLSSecretKey> skContributions) const
{
    return quorum.SetSecretKeyShare(m_bls_worker.AggregateSecretKeys(skContributions), nodeman->GetProTxHash());
}

size_t ActiveContext::GetQuorumRecoveryStartOffset(const llmq::CQuorum& quorum,
                                                   gsl::not_null<const CBlockIndex*> pIndex) const
{
    auto mns = m_dmnman.GetListForBlock(pIndex);
    std::vector<uint256> vecProTxHashes;
    vecProTxHashes.reserve(mns.GetCounts().enabled());
    mns.ForEachMN(/*onlyValid=*/true,
                  [&](const auto& pMasternode) { vecProTxHashes.emplace_back(pMasternode.proTxHash); });
    std::sort(vecProTxHashes.begin(), vecProTxHashes.end());
    size_t nIndex{0};
    {
        auto my_protx_hash = nodeman->GetProTxHash();
        for (const auto i : util::irange(vecProTxHashes.size())) {
            // cppcheck-suppress useStlAlgorithm
            if (my_protx_hash == vecProTxHashes[i]) {
                nIndex = i;
                break;
            }
        }
    }
    return nIndex % quorum.qc->validMembers.size();
}

MessageProcessingResult ActiveContext::ProcessContribQGETDATA(bool request_limit_exceeded, CDataStream& vStream,
                                                              const llmq::CQuorum& quorum,
                                                              llmq::CQuorumDataRequest& request,
                                                              gsl::not_null<const CBlockIndex*> block_index)
{
    if (request.GetDataMask() & llmq::CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS) {
        assert(block_index);

        int memberIdx = quorum.GetMemberIndex(request.GetProTxHash());
        if (memberIdx == -1) {
            request.SetError(llmq::CQuorumDataRequest::Errors::MASTERNODE_IS_NO_MEMBER);
            return request_limit_exceeded ? MisbehavingError{25, "request limit exceeded"} : MessageProcessingResult{};
        }

        std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
        if (!m_qman.GetEncryptedContributions(request.GetLLMQType(), block_index,
                                              quorum.qc->validMembers, request.GetProTxHash(), vecEncrypted)) {
            request.SetError(llmq::CQuorumDataRequest::Errors::ENCRYPTED_CONTRIBUTIONS_MISSING);
            return request_limit_exceeded ? MisbehavingError{25, "request limit exceeded"} : MessageProcessingResult{};
        }

        vStream << vecEncrypted;
    }

    return {};
}

MessageProcessingResult ActiveContext::ProcessContribQDATA(CNode& pfrom, CDataStream& vStream,
                                                           llmq::CQuorum& quorum,
                                                           llmq::CQuorumDataRequest& request)
{
    if (request.GetDataMask() & llmq::CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS) {
        if (WITH_LOCK(quorum.cs_vvec_shShare, return !quorum.HasVerificationVectorInternal()
                                                     || quorum.quorumVvec->size() != size_t(quorum.params.threshold))) {
            // Don't bump score because we asked for it
            LogPrint(BCLog::LLMQ, "ActiveContext::%s -- %s: No valid quorum verification vector available, from peer=%d\n", __func__, NetMsgType::QDATA, pfrom.GetId());
            return {};
        }

        int memberIdx = quorum.GetMemberIndex(request.GetProTxHash());
        if (memberIdx == -1) {
            // Don't bump score because we asked for it
            LogPrint(BCLog::LLMQ, "ActiveContext::%s -- %s: Not a member of the quorum, from peer=%d\n", __func__, NetMsgType::QDATA, pfrom.GetId());
            return {};
        }

        std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
        vStream >> vecEncrypted;

        std::vector<CBLSSecretKey> vecSecretKeys;
        vecSecretKeys.resize(vecEncrypted.size());
        for (const auto i : util::irange(vecEncrypted.size())) {
            if (!nodeman->Decrypt(vecEncrypted[i], memberIdx, vecSecretKeys[i], PROTOCOL_VERSION)) {
                return MisbehavingError{10, "failed to decrypt"};
            }
        }

        if (!quorum.SetSecretKeyShare(m_bls_worker.AggregateSecretKeys(vecSecretKeys), nodeman->GetProTxHash())) {
            return MisbehavingError{10, "invalid secret key share received"};
        }
    }

    return {};
}

void ActiveContext::CheckQuorumConnections(const Consensus::LLMQParams& llmqParams,
                                           gsl::not_null<const CBlockIndex*> pindexNew) const
{
    auto lastQuorums = m_qman.ScanQuorums(llmqParams.type, pindexNew, (size_t)llmqParams.keepOldConnections);
    auto deletableQuorums = GetQuorumsToDelete(llmqParams, pindexNew);

    const uint256 proTxHash = nodeman->GetProTxHash();
    const bool watchOtherISQuorums = llmqParams.type == Params().GetConsensus().llmqTypeDIP0024InstantSend &&
                                     std::ranges::any_of(lastQuorums, [&proTxHash](const auto& old_quorum){ return old_quorum->IsMember(proTxHash); });

    for (const auto& quorum : lastQuorums) {
        if (llmq::utils::EnsureQuorumConnections(llmqParams, m_connman, m_sporkman, {m_dmnman, m_qsnapman, m_chainman, quorum->m_quorum_base_block_index},
                                           m_dmnman.GetListAtChainTip(), proTxHash, /*is_masternode=*/true, m_quorums_watch)) {
            if (deletableQuorums.erase(quorum->qc->quorumHash) > 0) {
                LogPrint(BCLog::LLMQ, "ActiveContext::%s -- llmqType[%d] h[%d] keeping mn quorum connections for quorum: [%d:%s]\n", __func__, std23::to_underlying(llmqParams.type), pindexNew->nHeight, quorum->m_quorum_base_block_index->nHeight, quorum->m_quorum_base_block_index->GetBlockHash().ToString());
            }
        } else if (watchOtherISQuorums && !quorum->IsMember(proTxHash)) {
            Uint256HashSet connections;
            const auto& cindexes = llmq::utils::CalcDeterministicWatchConnections(llmqParams.type, quorum->m_quorum_base_block_index, quorum->members.size(), 1);
            for (auto idx : cindexes) {
                connections.emplace(quorum->members[idx]->proTxHash);
            }
            if (!connections.empty()) {
                if (!m_connman.HasMasternodeQuorumNodes(llmqParams.type, quorum->m_quorum_base_block_index->GetBlockHash())) {
                    LogPrint(BCLog::LLMQ, "ActiveContext::%s -- llmqType[%d] h[%d] adding mn inter-quorum connections for quorum: [%d:%s]\n", __func__, std23::to_underlying(llmqParams.type), pindexNew->nHeight, quorum->m_quorum_base_block_index->nHeight, quorum->m_quorum_base_block_index->GetBlockHash().ToString());
                    m_connman.SetMasternodeQuorumNodes(llmqParams.type, quorum->m_quorum_base_block_index->GetBlockHash(), connections);
                    m_connman.SetMasternodeQuorumRelayMembers(llmqParams.type, quorum->m_quorum_base_block_index->GetBlockHash(), connections);
                }
                if (deletableQuorums.erase(quorum->qc->quorumHash) > 0) {
                    LogPrint(BCLog::LLMQ, "ActiveContext::%s -- llmqType[%d] h[%d] keeping mn inter-quorum connections for quorum: [%d:%s]\n", __func__, std23::to_underlying(llmqParams.type), pindexNew->nHeight, quorum->m_quorum_base_block_index->nHeight, quorum->m_quorum_base_block_index->GetBlockHash().ToString());
                }
            }
        }
    }

    for (const auto& quorumHash : deletableQuorums) {
        LogPrint(BCLog::LLMQ, "ActiveContext::%s -- removing masternodes quorum connections for quorum %s:\n", __func__, quorumHash.ToString());
        m_connman.RemoveMasternodeQuorumNodes(llmqParams.type, quorumHash);
    }
}

void ActiveContext::StartDataRecoveryThread(gsl::not_null<const CBlockIndex*> pIndex, llmq::CQuorumCPtr pQuorum,
                                            uint16_t nDataMaskIn) const
{
    bool expected = false;
    if (!pQuorum->fQuorumDataRecoveryThreadRunning.compare_exchange_strong(expected, true)) {
        LogPrint(BCLog::LLMQ, "ActiveContext::%s -- Already running\n", __func__);
        return;
    }

    workerPool.push([pQuorum = std::move(pQuorum), pIndex, nDataMaskIn, this](int threadId) mutable {
        const size_t size_offset = GetQuorumRecoveryStartOffset(*pQuorum, pIndex);
        DataRecoveryThread(pIndex, std::move(pQuorum), nDataMaskIn, nodeman->GetProTxHash(), size_offset);
    });
}

void ActiveContext::TriggerQuorumDataRecoveryThreads(gsl::not_null<const CBlockIndex*> block_index) const
{
    if (!m_quorums_recovery) {
        return;
    }

    LogPrint(BCLog::LLMQ, "ActiveContext::%s -- Process block %s\n", __func__, block_index->GetBlockHash().ToString());

    const uint256 proTxHash = nodeman->GetProTxHash();

    for (const auto& params : Params().GetConsensus().llmqs) {
        auto vecQuorums = m_qman.ScanQuorums(params.type, block_index, params.keepOldConnections);
        const bool fWeAreQuorumTypeMember = std::ranges::any_of(vecQuorums, [&proTxHash](const auto& pQuorum) {
            return pQuorum->IsValidMember(proTxHash);
        });

        for (auto& pQuorum : vecQuorums) {
            if (pQuorum->IsValidMember(proTxHash)) {
                uint16_t nDataMask{0};
                if (!pQuorum->HasVerificationVector()) {
                    nDataMask |= llmq::CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR;
                }
                if (!pQuorum->GetSkShare().IsValid()) {
                    nDataMask |= llmq::CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS;
                }
                if (nDataMask != 0) {
                    StartDataRecoveryThread(block_index, std::move(pQuorum), nDataMask);
                } else {
                    LogPrint(BCLog::LLMQ, "ActiveContext::%s -- No data needed from (%d, %s) at height %d\n", __func__,
                             std23::to_underlying(pQuorum->qc->llmqType), pQuorum->qc->quorumHash.ToString(), block_index->nHeight);
                }
            } else {
                TryStartVvecSyncThread(block_index, std::move(pQuorum), fWeAreQuorumTypeMember);
            }
        }
    }
}
