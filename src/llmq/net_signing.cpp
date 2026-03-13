// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/net_signing.h>

#include <bls/bls_batchverifier.h>
#include <cxxtimer.hpp>
#include <llmq/quorums.h>
#include <llmq/signhash.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>
#include <logging.h>
#include <net.h>
#include <netmessagemaker.h>
#include <spork.h>
#include <streams.h>
#include <util/std23.h>
#include <util/thread.h>
#include <validationinterface.h>

#include <algorithm>
#include <ranges>
#include <unordered_map>

#include <llmq/commitment.h>
#include <llmq/quorumsman.h>
#include <validation.h>

namespace llmq {
void NetSigning::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    if (msg_type == NetMsgType::QSIGREC) {
        auto recoveredSig = std::make_shared<CRecoveredSig>();
        vRecv >> *recoveredSig;

        WITH_LOCK(cs_main, m_peer_manager->PeerEraseObjectRequest(pfrom.GetId(), CInv{MSG_QUORUM_RECOVERED_SIG,
                                                                                      recoveredSig->GetHash()}));

        if (!Params().GetLLMQ(recoveredSig->getLlmqType()).has_value()) {
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 100);
            return;
        }

        m_sig_manager.VerifyAndProcessRecoveredSig(pfrom.GetId(), std::move(recoveredSig));
    }

    if (m_shares_manager == nullptr) return;

    if (m_sporkman.IsSporkActive(SPORK_21_QUORUM_ALL_CONNECTED) && msg_type == NetMsgType::QSIGSHARE) {
        std::vector<CSigShare> receivedSigShares;
        vRecv >> receivedSigShares;

        if (receivedSigShares.size() > CSigSharesManager::MAX_MSGS_SIG_SHARES) {
            LogPrint(BCLog::LLMQ_SIGS, "NetSigning::%s -- too many sigs in QSIGSHARE message. cnt=%d, max=%d, node=%d\n",
                     __func__, receivedSigShares.size(), CSigSharesManager::MAX_MSGS_SIG_SHARES, pfrom.GetId());
            BanNode(pfrom.GetId());
            return;
        }

        for (const auto& sigShare : receivedSigShares) {
            if (!m_shares_manager->ProcessMessageSigShare(pfrom.GetId(), sigShare)) {
                BanNode(pfrom.GetId());
            }
        }
    }

    if (msg_type == NetMsgType::QSIGSESANN) {
        std::vector<CSigSesAnn> msgs;
        vRecv >> msgs;
        if (msgs.size() > CSigSharesManager::MAX_MSGS_CNT_QSIGSESANN) {
            LogPrint(BCLog::LLMQ_SIGS, /* Continued */
                     "NetSigning::%s -- too many announcements in QSIGSESANN message. cnt=%d, max=%d, node=%d\n",
                     __func__, msgs.size(), CSigSharesManager::MAX_MSGS_CNT_QSIGSESANN, pfrom.GetId());
            BanNode(pfrom.GetId());
            return;
        }
        if (!std::ranges::all_of(msgs, [this, &pfrom](const auto& ann) {
                return m_shares_manager->ProcessMessageSigSesAnn(pfrom, ann);
            })) {
            BanNode(pfrom.GetId());
            return;
        }
    } else if (msg_type == NetMsgType::QSIGSHARESINV || msg_type == NetMsgType::QGETSIGSHARES) {
        std::vector<CSigSharesInv> msgs;
        vRecv >> msgs;
        if (msgs.size() > CSigSharesManager::MAX_MSGS_CNT_QSIGSHARES) {
            LogPrint(BCLog::LLMQ_SIGS, "NetSigning::%s -- too many invs in %s message. cnt=%d, max=%d, node=%d\n",
                     __func__, msg_type, msgs.size(), CSigSharesManager::MAX_MSGS_CNT_QSIGSHARES, pfrom.GetId());
            BanNode(pfrom.GetId());
            return;
        }
        if (!std::ranges::all_of(msgs, [this, &pfrom, &msg_type](const auto& inv) {
                return m_shares_manager->ProcessMessageSigShares(pfrom, inv, msg_type);
            })) {
            BanNode(pfrom.GetId());
            return;
        }
    } else if (msg_type == NetMsgType::QBSIGSHARES) {
        std::vector<CBatchedSigShares> msgs;
        vRecv >> msgs;
        const size_t totalSigsCount = std23::ranges::fold_left(msgs, size_t{0}, [](size_t s, const auto& bs) {
            return s + bs.sigShares.size();
        });
        if (totalSigsCount > CSigSharesManager::MAX_MSGS_TOTAL_BATCHED_SIGS) {
            LogPrint(BCLog::LLMQ_SIGS, "NetSigning::%s -- too many sigs in QBSIGSHARES message. cnt=%d, max=%d, node=%d\n",
                     __func__, msgs.size(), CSigSharesManager::MAX_MSGS_TOTAL_BATCHED_SIGS, pfrom.GetId());
            BanNode(pfrom.GetId());
            return;
        }
        if (!std::ranges::all_of(msgs, [this, &pfrom](const auto& bs) {
                return m_shares_manager->ProcessMessageBatchedSigShares(pfrom, bs);
            })) {
            BanNode(pfrom.GetId());
            return;
        }
    }
}

void NetSigning::Start()
{
    // can't start new thread if we have one running already
    assert(!signing_thread.joinable());
    assert(!shares_cleaning_thread.joinable());
    assert(!shares_dispatcher_thread.joinable());

    signing_thread = std::thread(&util::TraceThread, "recsigs", [this] { WorkThreadSigning(); });

    if (m_shares_manager) {
        // Initialize worker pool
        int worker_count = std::clamp(static_cast<int>(std::thread::hardware_concurrency() / 2), 1, 4);
        worker_pool.resize(worker_count);
        RenameThreadPool(worker_pool, "sigsh-work");

        shares_cleaning_thread = std::thread(&util::TraceThread, "sigsh-maint", [this] { WorkThreadCleaning(); });
        shares_dispatcher_thread = std::thread(&util::TraceThread, "sigsh-dispat", [this] { WorkThreadDispatcher(); });
    }
}

void NetSigning::Stop()
{
    // make sure to call InterruptWorkerThread() first
    if (!workInterrupt) {
        assert(false);
    }

    if (signing_thread.joinable()) {
        signing_thread.join();
    }

    if (m_shares_manager) {
        // Join threads FIRST to stop any pending push() calls
        if (shares_cleaning_thread.joinable()) {
            shares_cleaning_thread.join();
        }
        if (shares_dispatcher_thread.joinable()) {
            shares_dispatcher_thread.join();
        }

        // Then stop worker pool (now safe, no more push() calls)
        worker_pool.clear_queue();
        worker_pool.stop(true);
    }
}

void NetSigning::ProcessRecoveredSig(std::shared_ptr<const CRecoveredSig> recovered_sig, bool consider_proactive_relay)
{
    if (recovered_sig == nullptr) return;
    if (!m_sig_manager.ProcessRecoveredSig(recovered_sig)) return;

    auto listeners = m_sig_manager.GetListeners();
    for (auto& l : listeners) {
        m_peer_manager->PeerPostProcessMessage(l->HandleNewRecoveredSig(*recovered_sig));
    }

    // TODO refactor to use a better abstraction analogous to IsAllMembersConnectedEnabled
    auto proactive_relay = consider_proactive_relay && recovered_sig->getLlmqType() != Consensus::LLMQType::LLMQ_100_67 &&
                           recovered_sig->getLlmqType() != Consensus::LLMQType::LLMQ_400_60 &&
                           recovered_sig->getLlmqType() != Consensus::LLMQType::LLMQ_400_85;
    GetMainSignals().NotifyRecoveredSig(recovered_sig, recovered_sig->GetHash().ToString(), proactive_relay);
}

bool NetSigning::ProcessPendingRecoveredSigs()
{
    Uint256HashMap<std::shared_ptr<const CRecoveredSig>> pending{m_sig_manager.FetchPendingReconstructed()};

    for (const auto& p : pending) {
        ProcessRecoveredSig(p.second, true);
    }

    std::unordered_map<NodeId, std::list<std::shared_ptr<const CRecoveredSig>>> recSigsByNode;
    std::unordered_map<std::pair<Consensus::LLMQType, uint256>, CBLSPublicKey, StaticSaltedHasher> pubkeys;

    const size_t nMaxBatchSize{32};
    bool more_work = m_sig_manager.CollectPendingRecoveredSigsToVerify(nMaxBatchSize, recSigsByNode, pubkeys);
    if (recSigsByNode.empty()) {
        return false;
    }

    // It's ok to perform insecure batched verification here as we verify against the quorum public keys, which are not
    // craftable by individual entities, making the rogue public key attack impossible
    CBLSBatchVerifier<NodeId, uint256> batchVerifier(false, false);

    size_t verifyCount = 0;
    for (const auto& [nodeId, v] : recSigsByNode) {
        for (const auto& recSig : v) {
            // we didn't verify the lazy signature until now
            if (!recSig->sig.Get().IsValid()) {
                batchVerifier.badSources.emplace(nodeId);
                break;
            }

            const auto& pubkey = pubkeys.at(std::make_pair(recSig->getLlmqType(), recSig->getQuorumHash()));
            batchVerifier.PushMessage(nodeId, recSig->GetHash(), recSig->buildSignHash().Get(), recSig->sig.Get(), pubkey);
            verifyCount++;
        }
    }

    cxxtimer::Timer verifyTimer(true);
    batchVerifier.Verify();
    verifyTimer.stop();

    LogPrint(BCLog::LLMQ, "NetSigning::%s -- verified recovered sig(s). count=%d, vt=%d, nodes=%d\n", __func__,
             verifyCount, verifyTimer.count(), recSigsByNode.size());

    Uint256HashSet processed;
    for (const auto& [nodeId, v] : recSigsByNode) {
        if (batchVerifier.badSources.count(nodeId)) {
            LogPrint(BCLog::LLMQ, "NetSigning::%s -- invalid recSig from other node, banning peer=%d\n", __func__, nodeId);
            m_peer_manager->PeerMisbehaving(nodeId, 100);
            continue;
        }

        for (const auto& recSig : v) {
            if (!processed.emplace(recSig->GetHash()).second) {
                continue;
            }

            ProcessRecoveredSig(recSig, nodeId == -1);
        }
    }

    return more_work;
}

void NetSigning::WorkThreadSigning()
{
    while (!workInterrupt) {
        bool fMoreWork = ProcessPendingRecoveredSigs();

        constexpr auto CLEANUP_INTERVAL{5s};
        if (cleanupThrottler.TryCleanup(CLEANUP_INTERVAL)) {
            m_sig_manager.Cleanup();
        }

        // TODO Wakeup when pending signing is needed?
        if (!fMoreWork && !workInterrupt.sleep_for(std::chrono::milliseconds(100))) {
            return;
        }
    }
}

void NetSigning::RemoveBannedNodeStates()
{
    assert(m_shares_manager != nullptr);
    // Called regularly to cleanup local node states for banned nodes
    m_shares_manager->RemoveNodesIf([this](NodeId node_id) { return m_peer_manager->PeerIsBanned(node_id); });
}

void NetSigning::BanNode(NodeId nodeId)
{
    if (nodeId == -1) return;

    m_peer_manager->PeerMisbehaving(nodeId, 100);
    if (m_shares_manager) {
        m_shares_manager->MarkAsBanned(nodeId);
    }
}

void NetSigning::WorkThreadCleaning()
{
    assert(m_shares_manager);

    while (!workInterrupt) {
        RemoveBannedNodeStates();

        m_shares_manager->SendMessages();
        m_shares_manager->Cleanup();

        workInterrupt.sleep_for(std::chrono::milliseconds(100));
    }
}

void NetSigning::WorkThreadDispatcher()
{
    assert(m_shares_manager);

    while (!workInterrupt) {
        // Dispatch all pending signs (individual tasks)
        {
            auto signs = m_shares_manager->DispatchPendingSigns();
            // Dispatch all signs to worker pool
            for (auto& work : signs) {
                if (workInterrupt) break;

                worker_pool.push([this, work = std::move(work)](int) mutable {
                    auto rs = m_shares_manager->SignAndProcessSingleShare(std::move(work));
                    ProcessRecoveredSig(rs, true);
                });
            }
        }

        if (m_shares_manager->IsAnyPendingProcessing()) {
            // If there's processing work, spawn a helper worker
            worker_pool.push([this](int) {
                while (!workInterrupt) {
                    bool moreWork = ProcessPendingSigShares();

                    if (!moreWork) {
                        return; // No work found, exit immediately
                    }
                }
            });
        }

        // Always sleep briefly between checks
        workInterrupt.sleep_for(std::chrono::milliseconds(10));
    }
}

void NetSigning::NotifyRecoveredSig(const std::shared_ptr<const CRecoveredSig>& sig, bool proactive_relay)
{
    m_peer_manager->PeerRelayRecoveredSig(*sig, proactive_relay);
}

bool NetSigning::ProcessPendingSigShares()
{
    std::unordered_map<NodeId, std::vector<CSigShare>> sigSharesByNodes;
    std::unordered_map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr, StaticSaltedHasher> quorums;

    const size_t nMaxBatchSize{32};
    bool more_work = m_shares_manager->CollectPendingSigSharesToVerify(nMaxBatchSize, sigSharesByNodes, quorums);
    if (sigSharesByNodes.empty()) {
        return false;
    }

    // It's ok to perform insecure batched verification here as we verify against the quorum public key shares,
    // which are not craftable by individual entities, making the rogue public key attack impossible
    CBLSBatchVerifier<NodeId, SigShareKey> batchVerifier(false, true);

    cxxtimer::Timer prepareTimer(true);
    size_t verifyCount = 0;
    for (const auto& [nodeId, v] : sigSharesByNodes) {
        for (const auto& sigShare : v) {
            if (m_sig_manager.HasRecoveredSigForId(sigShare.getLlmqType(), sigShare.getId())) {
                continue;
            }

            // Materialize the signature once. Get() internally validates, so if it returns an invalid signature,
            // we know it's malformed. This avoids calling Get() twice (once for IsValid(), once for PushMessage).
            CBLSSignature sig = sigShare.sigShare.Get();
            // we didn't check this earlier because we use a lazy BLS signature and tried to avoid doing the expensive
            // deserialization in the message thread
            if (!sig.IsValid()) {
                BanNode(nodeId);
                // don't process any additional shares from this node
                break;
            }

            auto quorum = quorums.at(std::make_pair(sigShare.getLlmqType(), sigShare.getQuorumHash()));
            auto pubKeyShare = quorum->GetPubKeyShare(sigShare.getQuorumMember());

            if (!pubKeyShare.IsValid()) {
                // this should really not happen (we already ensured we have the quorum vvec,
                // so we should also be able to create all pubkey shares)
                LogPrintf("NetSigning::%s -- pubKeyShare is invalid, which should not be possible here\n", __func__);
                assert(false);
            }

            batchVerifier.PushMessage(nodeId, sigShare.GetKey(), sigShare.GetSignHash(), sig, pubKeyShare);
            verifyCount++;
        }
    }
    prepareTimer.stop();

    cxxtimer::Timer verifyTimer(true);
    batchVerifier.Verify();
    verifyTimer.stop();

    LogPrint(BCLog::LLMQ_SIGS, "NetSigning::%s -- verified sig shares. count=%d, pt=%d, vt=%d, nodes=%d\n", __func__,
             verifyCount, prepareTimer.count(), verifyTimer.count(), sigSharesByNodes.size());

    for (const auto& [nodeId, v] : sigSharesByNodes) {
        if (batchVerifier.badSources.count(nodeId) != 0) {
            LogPrint(BCLog::LLMQ_SIGS, "NetSigning::%s -- invalid sig shares from other node, banning peer=%d\n",
                     __func__, nodeId);
            // this will also cause re-requesting of the shares that were sent by this node
            BanNode(nodeId);
            continue;
        }

        auto rec_sigs = m_shares_manager->ProcessPendingSigShares(v, quorums);
        for (auto& rs : rec_sigs) {
            ProcessRecoveredSig(rs, true);
        }
    }

    return more_work;
}

// TODO: move it away, it's temporary here
bool CQuorumManager::RequestQuorumData(CNode* pfrom, CConnman& connman, const CQuorum& quorum, uint16_t nDataMask,
                                       const uint256& proTxHash) const
{
    if (pfrom == nullptr) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Invalid pfrom: nullptr\n", __func__);
        return false;
    }
    if (pfrom->GetVerifiedProRegTxHash().IsNull()) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- pfrom is not a verified masternode\n", __func__);
        return false;
    }
    const Consensus::LLMQType llmqType = quorum.qc->llmqType;
    if (!Params().GetLLMQ(llmqType).has_value()) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Invalid llmqType: %d\n", __func__, std23::to_underlying(llmqType));
        return false;
    }
    const CBlockIndex* pindex{quorum.m_quorum_base_block_index};
    if (pindex == nullptr) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Invalid m_quorum_base_block_index : nullptr\n", __func__);
        return false;
    }

    LOCK(cs_data_requests);
    const CQuorumDataRequestKey key(pfrom->GetVerifiedProRegTxHash(), true, pindex->GetBlockHash(), llmqType);
    const CQuorumDataRequest request(llmqType, pindex->GetBlockHash(), nDataMask, proTxHash);
    auto [old_pair, inserted] = mapQuorumDataRequests.emplace(key, request);
    if (!inserted) {
        if (old_pair->second.IsExpired(/*add_bias=*/true)) {
            old_pair->second = request;
        } else {
            LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Already requested\n", __func__);
            return false;
        }
    }
    LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- sending QGETDATA quorumHash[%s] llmqType[%d] proRegTx[%s]\n", __func__, key.quorumHash.ToString(),
             std23::to_underlying(key.llmqType), key.proRegTx.ToString());

    CNetMsgMaker msgMaker(pfrom->GetCommonVersion());
    connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::QGETDATA, request));

    return true;
}

MessageProcessingResult CQuorumManager::ProcessMessage(CNode& pfrom, CConnman& connman, std::string_view msg_type, CDataStream& vRecv)
{
    if (msg_type == NetMsgType::QGETDATA) {
        if (!IsMasternode() || (pfrom.GetVerifiedProRegTxHash().IsNull() && !pfrom.qwatch)) {
            return MisbehavingError{10, "not a verified masternode or a qwatch connection"};
        }

        CQuorumDataRequest request;
        vRecv >> request;

        auto sendQDATA = [&](CQuorumDataRequest::Errors nError,
                             bool request_limit_exceeded,
                             const CDataStream& body = CDataStream(SER_NETWORK, PROTOCOL_VERSION)) -> MessageProcessingResult {
            MessageProcessingResult ret{};
            switch (nError) {
                case (CQuorumDataRequest::Errors::NONE):
                case (CQuorumDataRequest::Errors::QUORUM_TYPE_INVALID):
                case (CQuorumDataRequest::Errors::QUORUM_BLOCK_NOT_FOUND):
                case (CQuorumDataRequest::Errors::QUORUM_NOT_FOUND):
                case (CQuorumDataRequest::Errors::MASTERNODE_IS_NO_MEMBER):
                case (CQuorumDataRequest::Errors::UNDEFINED):
                    if (request_limit_exceeded) ret = MisbehavingError{25, "request limit exceeded"};
                    break;
                case (CQuorumDataRequest::Errors::QUORUM_VERIFICATION_VECTOR_MISSING):
                case (CQuorumDataRequest::Errors::ENCRYPTED_CONTRIBUTIONS_MISSING):
                    // Do not punish limit exceed if we don't have the requested data
                    break;
            }
            request.SetError(nError);
            CDataStream ssResponse{SER_NETWORK, pfrom.GetCommonVersion()};
            ssResponse << request << body;
            connman.PushMessage(&pfrom, CNetMsgMaker(pfrom.GetCommonVersion()).Make(NetMsgType::QDATA, ssResponse));
            return ret;
        };

        bool request_limit_exceeded(false);
        {
            LOCK2(::cs_main, cs_data_requests);
            const CQuorumDataRequestKey key(pfrom.GetVerifiedProRegTxHash(), false, request.GetQuorumHash(), request.GetLLMQType());
            auto it = mapQuorumDataRequests.find(key);
            if (it == mapQuorumDataRequests.end()) {
                it = mapQuorumDataRequests.emplace(key, request).first;
            } else if (it->second.IsExpired(/*add_bias=*/false)) {
                it->second = request;
            } else {
                request_limit_exceeded = true;
            }
        }

        if (!Params().GetLLMQ(request.GetLLMQType()).has_value()) {
            return sendQDATA(CQuorumDataRequest::Errors::QUORUM_TYPE_INVALID, request_limit_exceeded);
        }

        const CBlockIndex* pQuorumBaseBlockIndex = WITH_LOCK(::cs_main, return m_chainman.m_blockman.LookupBlockIndex(request.GetQuorumHash()));
        if (pQuorumBaseBlockIndex == nullptr) {
            return sendQDATA(CQuorumDataRequest::Errors::QUORUM_BLOCK_NOT_FOUND, request_limit_exceeded);
        }

        const auto pQuorum = GetQuorum(request.GetLLMQType(), pQuorumBaseBlockIndex);
        if (pQuorum == nullptr) {
            return sendQDATA(CQuorumDataRequest::Errors::QUORUM_NOT_FOUND, request_limit_exceeded);
        }

        CDataStream ssResponseData(SER_NETWORK, pfrom.GetCommonVersion());

        // Check if request wants QUORUM_VERIFICATION_VECTOR data
        if (request.GetDataMask() & CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR) {
            if (!pQuorum->HasVerificationVector()) {
                return sendQDATA(CQuorumDataRequest::Errors::QUORUM_VERIFICATION_VECTOR_MISSING, request_limit_exceeded);
            }

            WITH_LOCK(pQuorum->cs_vvec_shShare, ssResponseData << *pQuorum->quorumVvec);
        }

        // Check if request wants ENCRYPTED_CONTRIBUTIONS data
        CQuorumDataRequest::Errors ret_err{CQuorumDataRequest::Errors::NONE};
        MessageProcessingResult qdata_ret{}, ret{};
        if (m_handler) {
            ret = m_handler->ProcessContribQGETDATA(request_limit_exceeded, ssResponseData, *pQuorum, request, pQuorumBaseBlockIndex);
            if (auto request_err = request.GetError(); request_err != CQuorumDataRequest::Errors::NONE &&
                                                       request_err != CQuorumDataRequest::Errors::UNDEFINED) {
                ret_err = request_err;
            }
        }
        // sendQDATA also pushes a message independent of the returned value
        if (ret_err != CQuorumDataRequest::Errors::NONE) {
            qdata_ret = sendQDATA(ret_err, request_limit_exceeded);
        } else {
            qdata_ret = sendQDATA(CQuorumDataRequest::Errors::NONE, request_limit_exceeded, ssResponseData);
        }
        return ret.empty() ? qdata_ret : ret;
    }

    if (msg_type == NetMsgType::QDATA) {
        if ((!IsMasternode() && !IsWatching()) || pfrom.GetVerifiedProRegTxHash().IsNull()) {
            return MisbehavingError{10, "not a verified masternode and -watchquorums is not enabled"};
        }

        CQuorumDataRequest request;
        vRecv >> request;

        {
            LOCK2(::cs_main, cs_data_requests);
            const CQuorumDataRequestKey key(pfrom.GetVerifiedProRegTxHash(), true, request.GetQuorumHash(), request.GetLLMQType());
            auto it = mapQuorumDataRequests.find(key);
            if (it == mapQuorumDataRequests.end()) {
                return MisbehavingError{10, "not requested"};
            }
            if (it->second.IsProcessed()) {
                return MisbehavingError(10, "already received");
            }
            if (request != it->second) {
                return MisbehavingError(10, "not like requested");
            }
            it->second.SetProcessed();
        }

        if (request.GetError() != CQuorumDataRequest::Errors::NONE) {
            LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- %s: Error %d (%s), from peer=%d\n", __func__, msg_type, request.GetError(), request.GetErrorString(), pfrom.GetId());
            return {};
        }

        CQuorumPtr pQuorum;
        {
            if (LOCK(m_cs_maps); !mapQuorumsCache[request.GetLLMQType()].get(request.GetQuorumHash(), pQuorum)) {
                // Don't bump score because we asked for it
                LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- %s: Quorum not found, from peer=%d\n", __func__, msg_type, pfrom.GetId());
                return {};
            }
        }

        // Check if request has QUORUM_VERIFICATION_VECTOR data
        if (request.GetDataMask() & CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR) {

            std::vector<CBLSPublicKey> verificationVector;
            vRecv >> verificationVector;

            if (pQuorum->SetVerificationVector(verificationVector)) {
                QueueQuorumForWarming(pQuorum);
            } else {
                return MisbehavingError{10, "invalid quorum verification vector"};
            }
        }

        // Check if request has ENCRYPTED_CONTRIBUTIONS data
        if (m_handler) {
            if (auto ret = m_handler->ProcessContribQDATA(pfrom, vRecv, *pQuorum, request); !ret.empty()) {
                return ret;
            }
        }

        WITH_LOCK(cs_db, pQuorum->WriteContributions(*db));
        return {};
    }

    return {};
}

} // namespace llmq
