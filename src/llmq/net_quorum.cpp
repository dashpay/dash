// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/net_quorum.h>

#include <active/masternode.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <llmq/commitment.h>
#include <llmq/options.h>
#include <llmq/quorumsman.h>
#include <llmq/utils.h>
#include <logging.h>
#include <masternode/sync.h>
#include <net.h>
#include <netmessagemaker.h>
#include <util/helpers.h>
#include <util/std23.h>
#include <util/thread.h>
#include <util/time.h>
#include <validation.h>

#include <cxxtimer.hpp>

#include <algorithm>
#include <ranges>

namespace llmq {

NetQuorum::NetQuorum(PeerManagerInternal* peer_manager, CBLSWorker& bls_worker,
                     CConnman& connman, CDeterministicMNManager& dmnman, CQuorumManager& qman,
                     CQuorumSnapshotManager& qsnapman, const ChainstateManager& chainman,
                     const CMasternodeSync& mn_sync, const CSporkManager& sporkman,
                     QuorumRole* quorum_role, CActiveMasternodeManager* nodeman,
                     int16_t worker_count, const QvvecSyncModeMap& sync_map, bool quorums_recovery) :
    NetHandler(peer_manager),
    m_bls_worker{bls_worker},
    m_connman{connman},
    m_dmnman{dmnman},
    m_qman{qman},
    m_qsnapman{qsnapman},
    m_chainman{chainman},
    m_mn_sync{mn_sync},
    m_sporkman{sporkman},
    m_role{quorum_role},
    m_nodeman{nodeman},
    m_worker_count{worker_count},
    m_sync_map{sync_map},
    m_quorums_recovery{quorums_recovery}
{
    quorumThreadInterrupt.reset();
}

// NetHandler

void NetQuorum::Start()
{
    assert(m_worker_count > 0);
    workerPool.resize(m_worker_count);
    RenameThreadPool(workerPool, "q-mngr");
}

void NetQuorum::Stop()
{
    workerPool.clear_queue();
    workerPool.stop(true);
}

void NetQuorum::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    if (msg_type == NetMsgType::QGETDATA) {
        if (!m_role || !m_role->IsMasternode() || (pfrom.GetVerifiedProRegTxHash().IsNull() && !pfrom.qwatch)) {
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10, "not a verified masternode or a qwatch connection");
            return;
        }

        CQuorumDataRequest request;
        vRecv >> request;

        auto sendQDATA = [&](CQuorumDataRequest::Errors nError,
                             bool request_limit_exceeded,
                             const CDataStream& body = CDataStream(SER_NETWORK, PROTOCOL_VERSION)) -> bool {
            bool misbehave = false;
            switch (nError) {
                case (CQuorumDataRequest::Errors::NONE):
                case (CQuorumDataRequest::Errors::QUORUM_TYPE_INVALID):
                case (CQuorumDataRequest::Errors::QUORUM_BLOCK_NOT_FOUND):
                case (CQuorumDataRequest::Errors::QUORUM_NOT_FOUND):
                case (CQuorumDataRequest::Errors::MASTERNODE_IS_NO_MEMBER):
                case (CQuorumDataRequest::Errors::UNDEFINED):
                    misbehave = request_limit_exceeded;
                    break;
                case (CQuorumDataRequest::Errors::QUORUM_VERIFICATION_VECTOR_MISSING):
                case (CQuorumDataRequest::Errors::ENCRYPTED_CONTRIBUTIONS_MISSING):
                    // Do not punish limit exceed if we don't have the requested data
                    break;
            }
            request.SetError(nError);
            CDataStream ssResponse{SER_NETWORK, pfrom.GetCommonVersion()};
            ssResponse << request << body;
            m_connman.PushMessage(&pfrom, CNetMsgMaker(pfrom.GetCommonVersion()).Make(NetMsgType::QDATA, ssResponse));
            return misbehave;
        };

        const CQuorumDataRequestKey key(pfrom.GetVerifiedProRegTxHash(), false, request.GetQuorumHash(), request.GetLLMQType());
        const bool request_limit_exceeded = !m_qman.RegisterDataRequest(key, request, /*add_expiry_bias=*/false);

        if (!Params().GetLLMQ(request.GetLLMQType()).has_value()) {
            if (sendQDATA(CQuorumDataRequest::Errors::QUORUM_TYPE_INVALID, request_limit_exceeded)) {
                m_peer_manager->PeerMisbehaving(pfrom.GetId(), 25, "request limit exceeded");
            }
            return;
        }

        const CBlockIndex* pQuorumBaseBlockIndex = WITH_LOCK(::cs_main, return m_chainman.m_blockman.LookupBlockIndex(request.GetQuorumHash()));
        if (pQuorumBaseBlockIndex == nullptr) {
            if (sendQDATA(CQuorumDataRequest::Errors::QUORUM_BLOCK_NOT_FOUND, request_limit_exceeded)) {
                m_peer_manager->PeerMisbehaving(pfrom.GetId(), 25, "request limit exceeded");
            }
            return;
        }

        const auto pQuorum = m_qman.GetQuorum(request.GetLLMQType(), request.GetQuorumHash());
        if (pQuorum == nullptr) {
            if (sendQDATA(CQuorumDataRequest::Errors::QUORUM_NOT_FOUND, request_limit_exceeded)) {
                m_peer_manager->PeerMisbehaving(pfrom.GetId(), 25, "request limit exceeded");
            }
            return;
        }

        CDataStream ssResponseData(SER_NETWORK, pfrom.GetCommonVersion());

        // Check if request wants QUORUM_VERIFICATION_VECTOR data
        if (request.GetDataMask() & CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR) {
            if (!pQuorum->HasVerificationVector()) {
                if (sendQDATA(CQuorumDataRequest::Errors::QUORUM_VERIFICATION_VECTOR_MISSING, request_limit_exceeded)) {
                    m_peer_manager->PeerMisbehaving(pfrom.GetId(), 25, "request limit exceeded");
                }
                return;
            }
            ssResponseData << *pQuorum->GetVerificationVector();
        }

        // Check if request wants ENCRYPTED_CONTRIBUTIONS data
        bool misbehave_contrib = ProcessContribQGETDATA(ssResponseData, *pQuorum, request, pQuorumBaseBlockIndex);

        CQuorumDataRequest::Errors ret_err{CQuorumDataRequest::Errors::NONE};
        if (auto request_err = request.GetError(); request_err != CQuorumDataRequest::Errors::NONE &&
                                                   request_err != CQuorumDataRequest::Errors::UNDEFINED) {
            ret_err = request_err;
        }

        bool misbehave_qdata = (ret_err != CQuorumDataRequest::Errors::NONE)
            ? sendQDATA(ret_err, request_limit_exceeded)
            : sendQDATA(CQuorumDataRequest::Errors::NONE, request_limit_exceeded, ssResponseData);

        if (request_limit_exceeded && (misbehave_contrib || misbehave_qdata)) {
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 25, "request limit exceeded");
        }
        return;
    }

    if (msg_type == NetMsgType::QDATA) {
        if (!m_role || pfrom.GetVerifiedProRegTxHash().IsNull()) {
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10, "not a verified masternode and -watchquorums is not enabled");
            return;
        }

        CQuorumDataRequest request;
        vRecv >> request;

        {
            const CQuorumDataRequestKey key(pfrom.GetVerifiedProRegTxHash(), true, request.GetQuorumHash(), request.GetLLMQType());
            const auto validation = m_qman.ValidateDataResponse(key, request);
            switch (validation) {
            case CQuorumManager::DataResponseValidation::NotRequested:
                m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10, "not requested");
                return;
            case CQuorumManager::DataResponseValidation::AlreadyReceived:
                m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10, "already received");
                return;
            case CQuorumManager::DataResponseValidation::Mismatch:
                m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10, "not like requested");
                return;
            case CQuorumManager::DataResponseValidation::OK:
                break;
            }
        }

        if (request.GetError() != CQuorumDataRequest::Errors::NONE) {
            LogPrint(BCLog::LLMQ, "NetQuorum::%s -- %s: Error %d (%s), from peer=%d\n", __func__, msg_type, request.GetError(), request.GetErrorString(), pfrom.GetId());
            return;
        }

        CQuorumPtr pQuorum = m_qman.GetCachedMutableQuorum(request.GetLLMQType(), request.GetQuorumHash());
        if (!pQuorum) {
            // Don't bump score because we asked for it
            LogPrint(BCLog::LLMQ, "NetQuorum::%s -- %s: Quorum not found, from peer=%d\n", __func__, msg_type, pfrom.GetId());
            return;
        }

        // Check if request has QUORUM_VERIFICATION_VECTOR data
        if (request.GetDataMask() & CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR) {
            std::vector<CBLSPublicKey> verificationVector;
            vRecv >> verificationVector;

            if (pQuorum->SetVerificationVector(verificationVector)) {
                m_qman.QueueQuorumForWarming(pQuorum);
            } else {
                m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10, "invalid quorum verification vector");
                return;
            }
        }

        // Check if request has ENCRYPTED_CONTRIBUTIONS data
        if (!ProcessContribQDATA(pfrom, vRecv, *pQuorum, request)) {
            return;
        }

        m_qman.WriteContributions(pQuorum);
    }
}

bool NetQuorum::ProcessContribQGETDATA(CDataStream& ssResponseData, const CQuorum& quorum,
                                       CQuorumDataRequest& request,
                                       gsl::not_null<const CBlockIndex*> block_index) const
{
    if (!(request.GetDataMask() & CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS)) {
        return false;
    }

    if (!m_nodeman) {
        request.SetError(CQuorumDataRequest::Errors::ENCRYPTED_CONTRIBUTIONS_MISSING);
        return true;
    }

    int memberIdx = quorum.GetMemberIndex(request.GetProTxHash());
    if (memberIdx == -1) {
        request.SetError(CQuorumDataRequest::Errors::MASTERNODE_IS_NO_MEMBER);
        return true;
    }

    std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
    if (!m_qman.GetEncryptedContributions(request.GetLLMQType(), block_index,
                                         quorum.qc->validMembers, request.GetProTxHash(), vecEncrypted)) {
        request.SetError(CQuorumDataRequest::Errors::ENCRYPTED_CONTRIBUTIONS_MISSING);
        return true;
    }

    ssResponseData << vecEncrypted;
    return false;
}

bool NetQuorum::ProcessContribQDATA(CNode& pfrom, CDataStream& vRecv,
                                    CQuorum& quorum, CQuorumDataRequest& request)
{
    if (!(request.GetDataMask() & CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS)) {
        return true;
    }

    if (!m_nodeman) {
        return true;
    }

    auto vvec = quorum.GetVerificationVector();
    if (!vvec || vvec->size() != size_t(quorum.params.threshold)) {
        LogPrint(BCLog::LLMQ, "NetQuorum::%s -- %s: No valid quorum verification vector available, from peer=%d\n",
                 __func__, NetMsgType::QDATA, pfrom.GetId());
        return false;
    }

    int memberIdx = quorum.GetMemberIndex(request.GetProTxHash());
    if (memberIdx == -1) {
        LogPrint(BCLog::LLMQ, "NetQuorum::%s -- %s: Not a member of the quorum, from peer=%d\n",
                 __func__, NetMsgType::QDATA, pfrom.GetId());
        return false;
    }

    std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
    vRecv >> vecEncrypted;

    std::vector<CBLSSecretKey> vecSecretKeys;
    vecSecretKeys.resize(vecEncrypted.size());
    for (const auto i : util::irange(vecEncrypted.size())) {
        if (!m_nodeman->Decrypt(vecEncrypted[i], memberIdx, vecSecretKeys[i], PROTOCOL_VERSION)) {
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10, "failed to decrypt");
            return false;
        }
    }

    if (!quorum.SetSecretKeyShare(m_bls_worker.AggregateSecretKeys(vecSecretKeys),
                                  m_nodeman->GetProTxHash())) {
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10, "invalid secret key share received");
        return false;
    }

    return true;
}

DataRequestStatus NetQuorum::RequestQuorumData(CNode& peer, const CQuorum& quorum, uint16_t nDataMask,
                                               const uint256& proTxHash) const
{
    const CQuorumDataRequestKey key(peer.GetVerifiedProRegTxHash(), true,
                                    quorum.m_quorum_base_block_index->GetBlockHash(), quorum.qc->llmqType);
    const CQuorumDataRequest request(quorum.qc->llmqType, quorum.m_quorum_base_block_index->GetBlockHash(),
                                     nDataMask, proTxHash);
    if (!m_qman.RegisterDataRequest(key, request)) {
        return m_qman.GetDataRequestStatus(peer.GetVerifiedProRegTxHash(), /*we_requested=*/true,
                                           quorum.m_quorum_base_block_index->GetBlockHash(), quorum.qc->llmqType);
    }
    LogPrint(BCLog::LLMQ, "NetQuorum::%s -- sending QGETDATA quorumHash[%s] llmqType[%d] proRegTx[%s]\n", __func__,
             key.quorumHash.ToString(), std23::to_underlying(key.llmqType), key.proRegTx.ToString());

    CNetMsgMaker msgMaker(peer.GetCommonVersion());
    m_connman.PushMessage(&peer, msgMaker.Make(NetMsgType::QGETDATA, request));
    return DataRequestStatus::Requested;
}


void NetQuorum::InitializeCurrentBlockTip(const CBlockIndex* tip, bool ibd)
{
    if (!m_role) return;
    UpdatedBlockTip(tip, nullptr, ibd);
    if (tip) {
        for (const auto& params : Params().GetConsensus().llmqs) {
            CheckQuorumConnections(params, tip);
        }
    }
}

// CValidationInterface

void NetQuorum::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (!m_role) return;
    if (!pindexNew) return;
    if (fInitialDownload || pindexNew == pindexFork) return;
    if (!m_mn_sync.IsBlockchainSynced()) return;

    for (const auto& params : Params().GetConsensus().llmqs) {
        CheckQuorumConnections(params, pindexNew);
    }

    m_qman.CleanupExpiredDataRequests();
    TriggerQuorumDataRecoveryThreads(pindexNew);
    StartCleanupOldQuorumDataThread(pindexNew);
}

// Private helpers

Uint256HashSet NetQuorum::GetQuorumsToDelete(const Consensus::LLMQParams& llmqParams,
                                             gsl::not_null<const CBlockIndex*> pindexNew) const
{
    auto connmanQuorumsToDelete = m_connman.GetMasternodeQuorums(llmqParams.type);

    if (IsQuorumRotationEnabled(llmqParams, pindexNew)) {
        int cycleIndexTipHeight = pindexNew->nHeight % llmqParams.dkgInterval;
        int cycleQuorumBaseHeight = pindexNew->nHeight - cycleIndexTipHeight;
        std::stringstream ss;
        for (const auto quorumIndex : util::irange(llmqParams.signingActiveQuorumCount)) {
            if (quorumIndex <= cycleIndexTipHeight) {
                int curDkgHeight = cycleQuorumBaseHeight + quorumIndex;
                auto curDkgBlock = pindexNew->GetAncestor(curDkgHeight)->GetBlockHash();
                ss << curDkgHeight << ":" << curDkgBlock.ToString() << " | ";
                connmanQuorumsToDelete.erase(curDkgBlock);
            }
        }
        LogPrint(BCLog::LLMQ, "NetQuorum::%s -- llmqType[%d] h[%d] keeping mn quorum connections for rotated quorums: [%s]\n",
                 __func__, std23::to_underlying(llmqParams.type), pindexNew->nHeight, ss.str());
    } else {
        int curDkgHeight = pindexNew->nHeight - (pindexNew->nHeight % llmqParams.dkgInterval);
        auto curDkgBlock = pindexNew->GetAncestor(curDkgHeight)->GetBlockHash();
        connmanQuorumsToDelete.erase(curDkgBlock);
        LogPrint(BCLog::LLMQ, "NetQuorum::%s -- llmqType[%d] h[%d] keeping mn quorum connections for quorum: [%d:%s]\n",
                 __func__, std23::to_underlying(llmqParams.type), pindexNew->nHeight, curDkgHeight, curDkgBlock.ToString());
    }

    return connmanQuorumsToDelete;
}

void NetQuorum::CheckQuorumConnections(const Consensus::LLMQParams& llmqParams,
                                                gsl::not_null<const CBlockIndex*> pindexNew) const
{
    const bool is_masternode = m_role->IsMasternode();
    const uint256 proTxHash = is_masternode ? m_role->GetProTxHash() : uint256{};

    auto lastQuorums = m_qman.ScanQuorums(llmqParams.type, pindexNew, (size_t)llmqParams.keepOldConnections);
    auto deletableQuorums = GetQuorumsToDelete(llmqParams, pindexNew);

    const bool watchOtherISQuorums = is_masternode &&
        llmqParams.type == Params().GetConsensus().llmqTypeDIP0024InstantSend &&
        std::ranges::any_of(lastQuorums, [&proTxHash](const auto& old_quorum) { return old_quorum->IsMember(proTxHash); });

    for (const auto& quorum : lastQuorums) {
        if (utils::EnsureQuorumConnections(llmqParams, m_connman, m_sporkman,
                                           {m_dmnman, m_qsnapman, m_chainman, quorum->m_quorum_base_block_index},
                                           m_dmnman.GetListAtChainTip(), proTxHash,
                                           /*is_masternode=*/is_masternode,
                                           /*quorums_watch=*/is_masternode ? m_role->IsWatching() : true)) {
            if (deletableQuorums.erase(quorum->qc->quorumHash) > 0) {
                LogPrint(BCLog::LLMQ, "NetQuorum::%s -- llmqType[%d] h[%d] keeping mn quorum connections for quorum: [%d:%s]\n",
                         __func__, std23::to_underlying(llmqParams.type), pindexNew->nHeight,
                         quorum->m_quorum_base_block_index->nHeight,
                         quorum->m_quorum_base_block_index->GetBlockHash().ToString());
            }
        } else if (watchOtherISQuorums && !quorum->IsMember(proTxHash)) {
            Uint256HashSet connections;
            const auto& cindexes = utils::CalcDeterministicWatchConnections(llmqParams.type,
                                                                             quorum->m_quorum_base_block_index,
                                                                             quorum->members.size(), 1);
            for (auto idx : cindexes) {
                connections.emplace(quorum->members[idx]->proTxHash);
            }
            if (!connections.empty()) {
                if (!m_connman.HasMasternodeQuorumNodes(llmqParams.type, quorum->m_quorum_base_block_index->GetBlockHash())) {
                    LogPrint(BCLog::LLMQ, "NetQuorum::%s -- llmqType[%d] h[%d] adding mn inter-quorum connections for quorum: [%d:%s]\n",
                             __func__, std23::to_underlying(llmqParams.type), pindexNew->nHeight,
                             quorum->m_quorum_base_block_index->nHeight,
                             quorum->m_quorum_base_block_index->GetBlockHash().ToString());
                    m_connman.SetMasternodeQuorumNodes(llmqParams.type,
                                                       quorum->m_quorum_base_block_index->GetBlockHash(), connections);
                    m_connman.SetMasternodeQuorumRelayMembers(llmqParams.type,
                                                              quorum->m_quorum_base_block_index->GetBlockHash(), connections);
                }
                if (deletableQuorums.erase(quorum->qc->quorumHash) > 0) {
                    LogPrint(BCLog::LLMQ, "NetQuorum::%s -- llmqType[%d] h[%d] keeping mn inter-quorum connections for quorum: [%d:%s]\n",
                             __func__, std23::to_underlying(llmqParams.type), pindexNew->nHeight,
                             quorum->m_quorum_base_block_index->nHeight,
                             quorum->m_quorum_base_block_index->GetBlockHash().ToString());
                }
            }
        }
    }

    for (const auto& quorumHash : deletableQuorums) {
        LogPrint(BCLog::LLMQ, "NetQuorum::%s -- removing masternodes quorum connections for quorum %s:\n",
                 __func__, quorumHash.ToString());
        m_connman.RemoveMasternodeQuorumNodes(llmqParams.type, quorumHash);
    }
}

void NetQuorum::TriggerQuorumDataRecoveryThreads(gsl::not_null<const CBlockIndex*> block_index) const
{
    if (!m_quorums_recovery) return;

    bool is_masternode = m_role->IsMasternode();

    LogPrint(BCLog::LLMQ, "NetQuorum::%s -- Process block %s\n", __func__, block_index->GetBlockHash().ToString());

    const uint256 proTxHash = m_role->GetProTxHash();

    for (const auto& params : Params().GetConsensus().llmqs) {
        auto vecQuorums = m_qman.ScanQuorums(params.type, block_index, params.keepOldConnections);
        const bool fWeAreQuorumTypeMember = is_masternode &&std::ranges::any_of(vecQuorums, [&proTxHash](const auto& pQuorum) {
            return pQuorum->IsValidMember(proTxHash);
        });

        for (auto& pQuorum : vecQuorums) {
            if (is_masternode && pQuorum->IsValidMember(proTxHash)) {
                uint16_t nDataMask{0};
                if (!pQuorum->HasVerificationVector()) {
                    nDataMask |= CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR;
                }
                if (!pQuorum->GetSkShare().IsValid()) {
                    nDataMask |= CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS;
                }
                if (nDataMask != 0) {
                    StartSkShareRecoveryThread(block_index, std::move(pQuorum), nDataMask);
                } else {
                    LogPrint(BCLog::LLMQ, "NetQuorum::%s -- No data needed from (%d, %s) at height %d\n", __func__,
                             std23::to_underlying(pQuorum->qc->llmqType), pQuorum->qc->quorumHash.ToString(),
                             block_index->nHeight);
                }
            } else {
                TryStartVvecSyncThread(block_index, std::move(pQuorum), fWeAreQuorumTypeMember);
            }
        }
    }
}

void NetQuorum::DataRecoveryThread(gsl::not_null<const CBlockIndex*> block_index, CQuorumCPtr pQuorum,
                                   uint16_t data_mask, const uint256& protx_hash, size_t start_offset) const
{
    size_t nTries{0};
    uint16_t nDataMask{data_mask};
    int64_t nTimeLastSuccess{0};
    uint256* pCurrentMemberHash{nullptr};
    std::vector<uint256> vecMemberHashes;
    const int64_t nRequestTimeout{10};

    auto printLog = [&](const std::string& strMessage) {
        const std::string strMember{pCurrentMemberHash == nullptr ? "nullptr" : pCurrentMemberHash->ToString()};
        LogPrint(BCLog::LLMQ, "NetQuorum::DataRecoveryThread -- %s - for llmqType %d, quorumHash %s, nDataMask (%d/%d), pCurrentMemberHash %s, nTries %d\n",
                 strMessage, std23::to_underlying(pQuorum->qc->llmqType), pQuorum->qc->quorumHash.ToString(),
                 nDataMask, data_mask, strMember, nTries);
    };
    printLog("Start");

    while (!m_mn_sync.IsBlockchainSynced() && !quorumThreadInterrupt) {
        quorumThreadInterrupt.sleep_for(std::chrono::seconds(nRequestTimeout));
    }

    if (quorumThreadInterrupt) {
        printLog("Aborted");
        return;
    }

    vecMemberHashes.reserve(pQuorum->qc->validMembers.size());
    for (auto& member : pQuorum->members) {
        if (pQuorum->IsValidMember(member->proTxHash) && member->proTxHash != protx_hash) {
            vecMemberHashes.push_back(member->proTxHash);
        }
    }
    std::sort(vecMemberHashes.begin(), vecMemberHashes.end());

    printLog("Try to request");

    while (nDataMask > 0 && !quorumThreadInterrupt) {
        if (nDataMask & CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR &&
            pQuorum->HasVerificationVector()) {
            nDataMask &= ~CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR;
            printLog("Received quorumVvec");
        }

        if (nDataMask & CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS && pQuorum->GetSkShare().IsValid()) {
            nDataMask &= ~CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS;
            printLog("Received skShare");
        }

        if (nDataMask == 0) {
            printLog("Success");
            break;
        }

        if ((GetTime<std::chrono::seconds>().count() - nTimeLastSuccess) > nRequestTimeout) {
            if (nTries >= vecMemberHashes.size()) {
                printLog("All tried but failed");
                break;
            }
            pCurrentMemberHash = &vecMemberHashes[(start_offset + nTries++) % vecMemberHashes.size()];
            if (m_qman.IsDataRequestPending(*pCurrentMemberHash, /*we_requested=*/true, pQuorum->qc->quorumHash,
                                          pQuorum->qc->llmqType)) {
                printLog("Already asked");
                continue;
            }
            quorumThreadInterrupt.sleep_for(std::chrono::milliseconds(start_offset * 100));
            nTimeLastSuccess = GetTime<std::chrono::seconds>().count();
            m_connman.AddPendingMasternode(*pCurrentMemberHash);
            printLog("Connect");
        }

        m_connman.ForEachNode([&](CNode* pNode) {
            auto verifiedProRegTxHash = pNode->GetVerifiedProRegTxHash();
            if (pCurrentMemberHash == nullptr || verifiedProRegTxHash != *pCurrentMemberHash) {
                return;
            }

            switch (RequestQuorumData(*pNode, *pQuorum, nDataMask, protx_hash)) {
            case DataRequestStatus::Requested:
                nTimeLastSuccess = GetTime<std::chrono::seconds>().count();
                printLog("Requested");
                return;
            case DataRequestStatus::NotFound:
                printLog("Failed");
                pNode->fDisconnect = true;
                pCurrentMemberHash = nullptr;
                return;
            case DataRequestStatus::Processed:
                printLog("Processed");
                pNode->fDisconnect = true;
                pCurrentMemberHash = nullptr;
                return;
            case DataRequestStatus::Pending:
                printLog("Waiting");
                return;
            }
        });
        quorumThreadInterrupt.sleep_for(std::chrono::seconds(1));
    }
    pQuorum->ReleaseRecovery();
    printLog("Done");
}

void NetQuorum::StartVvecSyncThread(gsl::not_null<const CBlockIndex*> block_index, CQuorumCPtr pQuorum) const
{
    if (!pQuorum->TryClaimRecovery()) {
        LogPrint(BCLog::LLMQ, "NetQuorum::%s -- Already running\n", __func__);
        return;
    }

    workerPool.push([pQuorum = std::move(pQuorum), block_index, this](int threadId) mutable {
        DataRecoveryThread(block_index, std::move(pQuorum), CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR,
                           /*protx_hash=*/uint256(), /*start_offset=*/0);
    });
}

void NetQuorum::TryStartVvecSyncThread(gsl::not_null<const CBlockIndex*> block_index, CQuorumCPtr pQuorum,
                                       bool fWeAreQuorumTypeMember) const
{
    if (pQuorum->IsRecoveryRunning()) return;

    const bool fSyncForTypeEnabled = m_sync_map.count(pQuorum->qc->llmqType) > 0;
    const QvvecSyncMode syncMode = fSyncForTypeEnabled ? m_sync_map.at(pQuorum->qc->llmqType) : QvvecSyncMode::Invalid;
    const bool fSyncCurrent = syncMode == QvvecSyncMode::Always ||
                              (syncMode == QvvecSyncMode::OnlyIfTypeMember && fWeAreQuorumTypeMember);

    if ((fSyncForTypeEnabled && fSyncCurrent) && !pQuorum->HasVerificationVector()) {
        StartVvecSyncThread(block_index, std::move(pQuorum));
    } else {
        LogPrint(BCLog::LLMQ, "NetQuorum::%s -- No data needed from (%d, %s) at height %d\n", __func__,
                 std23::to_underlying(pQuorum->qc->llmqType), pQuorum->qc->quorumHash.ToString(), block_index->nHeight);
    }
}

void NetQuorum::StartSkShareRecoveryThread(gsl::not_null<const CBlockIndex*> pIndex, CQuorumCPtr pQuorum,
                                           uint16_t nDataMaskIn) const
{
    if (!pQuorum->TryClaimRecovery()) {
        LogPrint(BCLog::LLMQ, "NetQuorum::%s -- Already running\n", __func__);
        return;
    }

    workerPool.push([pQuorum = std::move(pQuorum), pIndex, nDataMaskIn, this](int threadId) mutable {
        const size_t size_offset = GetQuorumRecoveryStartOffset(*pQuorum, pIndex);
        DataRecoveryThread(pIndex, std::move(pQuorum), nDataMaskIn, m_role->GetProTxHash(), size_offset);
    });
}

size_t NetQuorum::GetQuorumRecoveryStartOffset(const CQuorum& quorum,
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
        auto my_protx_hash = m_role->GetProTxHash();
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

void NetQuorum::StartCleanupOldQuorumDataThread(gsl::not_null<const CBlockIndex*> pIndex) const
{
    // Note: this function is CPU heavy and we don't want it to be running during DKGs.
    // The largest dkgMiningWindowStart for a related quorum type is 42 (LLMQ_60_75).
    // At the same time most quorums use dkgInterval = 24 so the next DKG for them
    // (after block 576 + 42) will start at block 576 + 24 * 2. That's only a 6 blocks
    // window and it's better to have more room so we pick next cycle.
    // dkgMiningWindowStart for small quorums is 10 i.e. a safe block to start
    // these calculations is at height 576 + 24 * 2 + 10 = 576 + 58.
    if (pIndex->nHeight % 576 != 58) {
        return;
    }

    cxxtimer::Timer t(/*start=*/true);
    LogPrint(BCLog::LLMQ, "NetQuorum::%s -- start\n", __func__);

    // do not block the caller thread
    workerPool.push([pIndex, t, this](int threadId) {
        Uint256HashSet dbKeysToSkip;

        if (LOCK(cs_cleanup); cleanupQuorumsCache.empty()) {
            utils::InitQuorumsCache(cleanupQuorumsCache, m_chainman.GetConsensus(), /*limit_by_connections=*/false);
        }
        for (const auto& params : Params().GetConsensus().llmqs) {
            if (quorumThreadInterrupt) {
                break;
            }
            LOCK(cs_cleanup);
            auto& cache = cleanupQuorumsCache[params.type];
            const CBlockIndex* pindex_loop{pIndex};
            Uint256HashSet quorum_keys;
            while (pindex_loop != nullptr && pIndex->nHeight - pindex_loop->nHeight < params.max_store_depth()) {
                uint256 quorum_key;
                if (cache.get(pindex_loop->GetBlockHash(), quorum_key)) {
                    quorum_keys.insert(quorum_key);
                    if (quorum_keys.size() >= static_cast<size_t>(params.keepOldKeys)) break; // extra safety belt
                }
                pindex_loop = pindex_loop->pprev;
            }
            for (const auto& pQuorum : m_qman.ScanQuorums(params.type, pIndex, params.keepOldKeys - quorum_keys.size())) {
                const uint256 quorum_key = MakeQuorumKey(*pQuorum);
                quorum_keys.insert(quorum_key);
                cache.insert(pQuorum->m_quorum_base_block_index->GetBlockHash(), quorum_key);
            }
            dbKeysToSkip.merge(quorum_keys);
        }

        if (!quorumThreadInterrupt) {
            m_qman.CleanupOldQuorumData(dbKeysToSkip);
        }

        LogPrint(BCLog::LLMQ, "NetQuorum::StartCleanupOldQuorumDataThread -- done. time=%d\n", t.count());
    });
}

} // namespace llmq
