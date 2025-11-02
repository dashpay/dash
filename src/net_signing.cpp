// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <net_signing.h>

#include <bls/bls_batchverifier.h>
#include <cxxtimer.hpp>
#include <llmq/commitment.h>
#include <llmq/quorums.h>
#include <llmq/signhash.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>
#include <masternode/node.h>
#include <logging.h>
#include <spork.h>
#include <streams.h>
#include <util/thread.h>
#include <validationinterface.h>

#include <unordered_map>

using llmq::CSigSharesManager;

static bool PreVerifyRecoveredSig(Consensus::LLMQType& llmqType, const llmq::CQuorumManager& quorum_manager, const llmq::CRecoveredSig& recoveredSig)
{
    auto quorum = quorum_manager.GetQuorum(llmqType, recoveredSig.getQuorumHash());

    if (!quorum) {
        LogPrint(BCLog::LLMQ, "NetSigning::%s -- quorum %s not found\n", __func__,
                  recoveredSig.getQuorumHash().ToString());
        return false;
    }
    if (!llmq::IsQuorumActive(llmqType, quorum_manager, quorum->qc->quorumHash)) {
        return false;
    }

    return true;
}

void NetSigning::BanNode(NodeId id)
{
    if (id == -1) {
        return;
    }

    m_peer_manager->PeerMisbehaving(id, 100);
    if (m_shares_manager) {
        m_shares_manager->MarkAsBanned(id);
    }
}

void NetSigning::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    if (msg_type == NetMsgType::QSIGREC) {

        auto recoveredSig = std::make_shared<llmq::CRecoveredSig>();
        vRecv >> *recoveredSig;

        WITH_LOCK(cs_main, m_peer_manager->PeerEraseObjectRequest(pfrom.GetId(), CInv{MSG_QUORUM_RECOVERED_SIG, recoveredSig->GetHash()}));

        auto llmqType = recoveredSig->getLlmqType();
        if (!Params().GetLLMQ(llmqType).has_value()) {
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 100);
        }
        if (PreVerifyRecoveredSig(llmqType, m_sig_manager.Qman(), *recoveredSig)) {
            m_sig_manager.ProcessRecoveredSig(pfrom.GetId(), std::move(recoveredSig));
        }

        return;
    }
    // non-masternodes are not interested in sigshares
    if (m_shares_manager == nullptr) return;
    if (m_shares_manager->ActiveMNManager().GetProTxHash().IsNull()) return;

    CSigSharesManager& shares_manager{*m_shares_manager};

    if (msg_type == NetMsgType::QSIGSHARE) {
        // TODO: remove spork SPORK_21_QUORUM_ALL_CONNECTED
        if (!shares_manager.SporkManager().IsSporkActive(SPORK_21_QUORUM_ALL_CONNECTED)) return; // nothing to do

        std::vector<llmq::CSigShare> receivedSigShares;
        vRecv >> receivedSigShares;

        if (receivedSigShares.size() > CSigSharesManager::MAX_MSGS_SIG_SHARES) {
            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- too many sigs in QSIGSHARE message. cnt=%d, max=%d, node=%d\n", __func__, receivedSigShares.size(), CSigSharesManager::MAX_MSGS_SIG_SHARES, pfrom.GetId());
            BanNode(pfrom.GetId());
            return;
        }

        bool all_succeed = true;
        for (const auto& sigShare : receivedSigShares) {
            // If one share is invalid and node should be banned, no need to process other shares from the same node.
            if (!shares_manager.ProcessMessageSigShare(sigShare, pfrom.GetId()))
                all_succeed = false;
        }
        if (!all_succeed) {
            BanNode(pfrom.GetId());
        }
    } else if (msg_type == NetMsgType::QSIGSESANN) {
        std::vector<llmq::CSigSesAnn> msgs;
        vRecv >> msgs;
        if (msgs.size() > CSigSharesManager::MAX_MSGS_CNT_QSIGSESANN) {
            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- too many announcements in QSIGSESANN message. cnt=%d, max=%d, node=%d\n", __func__, msgs.size(), CSigSharesManager::MAX_MSGS_CNT_QSIGSESANN, pfrom.GetId());
            return BanNode(pfrom.GetId());
        }
        if (!ranges::all_of(msgs,
                            [&shares_manager, &pfrom](const auto& ann){ return shares_manager.ProcessMessageSigSesAnn(ann, pfrom.GetId()); })) {
            BanNode(pfrom.GetId());
        }
    } else if (msg_type == NetMsgType::QSIGSHARESINV) {
        std::vector<llmq::CSigSharesInv> msgs;
        vRecv >> msgs;
        if (msgs.size() > CSigSharesManager::MAX_MSGS_CNT_QSIGSHARESINV) {
            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- too many invs in QSIGSHARESINV message. cnt=%d, max=%d, node=%d\n", __func__, msgs.size(), CSigSharesManager::MAX_MSGS_CNT_QSIGSHARESINV, pfrom.GetId());
            BanNode(pfrom.GetId());
            return;
        }
        if (!ranges::all_of(msgs,
                            [&shares_manager, &pfrom](const auto& inv){ return shares_manager.ProcessMessageSigSharesInv(inv, pfrom.GetId()); })) {
            BanNode(pfrom.GetId());
        }
    } else if (msg_type == NetMsgType::QGETSIGSHARES) {
        std::vector<llmq::CSigSharesInv> msgs;
        vRecv >> msgs;
        if (msgs.size() > CSigSharesManager::MAX_MSGS_CNT_QGETSIGSHARES) {
            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- too many invs in QGETSIGSHARES message. cnt=%d, max=%d, node=%d\n", __func__, msgs.size(), CSigSharesManager::MAX_MSGS_CNT_QGETSIGSHARES, pfrom.GetId());
            BanNode(pfrom.GetId());
            return;
        }
        if (!ranges::all_of(msgs,
                            [&shares_manager, &pfrom](const auto& inv){ return shares_manager.ProcessMessageGetSigShares(pfrom, inv); })) {
            BanNode(pfrom.GetId());
            return;
        }
    } else if (msg_type == NetMsgType::QBSIGSHARES) {
        std::vector<llmq::CBatchedSigShares> msgs;
        vRecv >> msgs;
        size_t totalSigsCount = 0;
        for (const auto& bs : msgs) {
            totalSigsCount += bs.sigShares.size();
        }
        if (totalSigsCount > CSigSharesManager::MAX_MSGS_TOTAL_BATCHED_SIGS) {
            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- too many sigs in QBSIGSHARES message. cnt=%d, max=%d, node=%d\n", __func__, msgs.size(), CSigSharesManager::MAX_MSGS_TOTAL_BATCHED_SIGS, pfrom.GetId());
            BanNode(pfrom.GetId());
            return;
        }
        if (!ranges::all_of(msgs,
                            [&shares_manager, &pfrom](const auto& bs){ return shares_manager.ProcessMessageBatchedSigShares(pfrom, bs); })) {
            BanNode(pfrom.GetId());
            return;
        }
    }
}

void NetSigning::Start()
{
    // can't start new thread if we have one running already
    assert(!signing_thread.joinable());
    assert(!shares_thread.joinable());

    signing_thread = std::thread(&util::TraceThread, "recsigs", [this] { WorkThreadSigning(); });
    if (m_shares_manager) {
        shares_thread = std::thread(&util::TraceThread, "recsigs", [this] { WorkThreadShares(); });
    }
}

void NetSigning::Stop()
{
    // make sure to call InterruptWorkerThread() first
    if (!workInterrupt) {
        assert(false);
    }

    if (shares_thread.joinable()) {
        shares_thread.join();
    }
    if (signing_thread.joinable()) {
        signing_thread.join();
    }
}

void NetSigning::ProcessRecoveredSig(const std::shared_ptr<const llmq::CRecoveredSig>& recoveredSig)
{
    if (!m_sig_manager.ProcessRecoveredSig(recoveredSig)) return;

    auto listeners = m_sig_manager.GetListeners();
    for (auto& l : listeners) {
        // TODO: simplify it to std::variant<CInv, CTransaction, std::monostate>
        m_peer_manager->PeerPostProcessMessage(l->HandleNewRecoveredSig(*recoveredSig));
    }

    GetMainSignals().NotifyRecoveredSig(recoveredSig, recoveredSig->GetHash().ToString());
}

bool NetSigning::ProcessPendingRecoveredSigs()
{
    Uint256HashMap<std::shared_ptr<const llmq::CRecoveredSig>> pending{m_sig_manager.FetchPendingReconstructed()};

    for (const auto& p : pending) {
        ProcessRecoveredSig(p.second);
    }

    std::unordered_map<NodeId, std::list<std::shared_ptr<const llmq::CRecoveredSig>>> recSigsByNode;
    std::unordered_map<std::pair<Consensus::LLMQType, uint256>, llmq::CQuorumCPtr, StaticSaltedHasher> quorums;

    const size_t nMaxBatchSize{32};
    m_sig_manager.CollectPendingRecoveredSigsToVerify(nMaxBatchSize, recSigsByNode, quorums);
    if (recSigsByNode.empty()) {
        return false;
    }

    // It's ok to perform insecure batched verification here as we verify against the quorum public keys, which are not
    // craftable by individual entities, making the rogue public key attack impossible
    CBLSBatchVerifier<NodeId, uint256> batchVerifier(false, false);

    size_t verifyCount = 0;
    for (const auto& p : recSigsByNode) {
        NodeId nodeId = p.first;
        const auto& v = p.second;

        for (const auto& recSig : v) {
            // we didn't verify the lazy signature until now
            if (!recSig->sig.Get().IsValid()) {
                batchVerifier.badSources.emplace(nodeId);
                break;
            }

            const auto& quorum = quorums.at(std::make_pair(recSig->getLlmqType(), recSig->getQuorumHash()));
            batchVerifier.PushMessage(nodeId, recSig->GetHash(), recSig->buildSignHash().Get(), recSig->sig.Get(),
                                      quorum->qc->quorumPublicKey);
            verifyCount++;
        }
    }

    cxxtimer::Timer verifyTimer(true);
    batchVerifier.Verify();
    verifyTimer.stop();

    LogPrint(BCLog::LLMQ, "CSigningManager::%s -- verified recovered sig(s). count=%d, vt=%d, nodes=%d\n", __func__, verifyCount, verifyTimer.count(), recSigsByNode.size());

    Uint256HashSet processed;
    for (const auto& p : recSigsByNode) {
        NodeId nodeId = p.first;
        const auto& v = p.second;

        if (batchVerifier.badSources.count(nodeId)) {
            LogPrint(BCLog::LLMQ, "CSigningManager::%s -- invalid recSig from other node, banning peer=%d\n", __func__, nodeId);
            m_peer_manager->PeerMisbehaving(nodeId, 100);
            continue;
        }

        for (const auto& recSig : v) {
            if (!processed.emplace(recSig->GetHash()).second) {
                continue;
            }

            ProcessRecoveredSig(recSig);
        }
    }

    return recSigsByNode.size() >= nMaxBatchSize;
}

void NetSigning::WorkThreadSigning()
{
    while (!workInterrupt) {
        bool fMoreWork = ProcessPendingRecoveredSigs();

        m_sig_manager.Cleanup();

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
    std::vector<NodeId> nodes = m_shares_manager->GetAllNodes();
    for (NodeId node_id : nodes) {
        if (m_peer_manager->PeerIsBanned(node_id)) {
            m_shares_manager->RemoveAsBanned(node_id);
        }
    }
}

// TODO These 2 methods (ProcessPendingSigShares, ProcessPendingRecoveredSigs) have
// a lot in common; consider to refactor it to split common parts or make it template / parameter based
bool NetSigning::ProcessPendingSigShares()
{
    std::unordered_map<NodeId, std::vector<llmq::CSigShare>> sigSharesByNodes;
    std::unordered_map<std::pair<Consensus::LLMQType, uint256>, llmq::CQuorumCPtr, StaticSaltedHasher> quorums;

    const size_t nMaxBatchSize{32};
    bool collect_status = m_shares_manager->CollectPendingSigSharesToVerify(nMaxBatchSize, sigSharesByNodes, quorums);
    if (!collect_status || sigSharesByNodes.empty()) {
        return false;
    }

    // It's ok to perform insecure batched verification here as we verify against the quorum public key shares,
    // which are not craftable by individual entities, making the rogue public key attack impossible
    CBLSBatchVerifier<NodeId, llmq::SigShareKey> batchVerifier(false, true);

    cxxtimer::Timer prepareTimer(true);
    size_t verifyCount = 0;
    for (const auto& [nodeId, v] : sigSharesByNodes) {
        for (const auto& sigShare : v) {
            if (m_sig_manager.HasRecoveredSigForId(sigShare.getLlmqType(), sigShare.getId())) {
                continue;
            }

            // we didn't check this earlier because we use a lazy BLS signature and tried to avoid doing the expensive
            // deserialization in the message thread
            if (!sigShare.sigShare.Get().IsValid()) {
                BanNode(nodeId);
                // don't process any additional shares from this node
                break;
            }

            auto quorum = quorums.at(std::make_pair(sigShare.getLlmqType(), sigShare.getQuorumHash()));
            auto pubKeyShare = quorum->GetPubKeyShare(sigShare.getQuorumMember());

            if (!pubKeyShare.IsValid()) {
                // this should really not happen (we already ensured we have the quorum vvec,
                // so we should also be able to create all pubkey shares)
                LogPrintf("CSigSharesManager::%s -- pubKeyShare is invalid, which should not be possible here\n", __func__);
                assert(false);
            }

            batchVerifier.PushMessage(nodeId, sigShare.GetKey(), sigShare.GetSignHash(), sigShare.sigShare.Get(), pubKeyShare);
            verifyCount++;
        }
    }
    prepareTimer.stop();

    cxxtimer::Timer verifyTimer(true);
    batchVerifier.Verify();
    verifyTimer.stop();

    LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- verified sig shares. count=%d, pt=%d, vt=%d, nodes=%d\n", __func__, verifyCount, prepareTimer.count(), verifyTimer.count(), sigSharesByNodes.size());

    for (const auto& [nodeId, sigSharesToProcess] : sigSharesByNodes) {
        if (batchVerifier.badSources.count(nodeId) != 0) {
            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- invalid sig shares from other node, banning peer=%d\n",
                     __func__, nodeId);
            // this will also cause re-requesting of the shares that were sent by this node
            BanNode(nodeId);
            continue;
        }

        // It's ensured that no duplicates are passed to this method
        cxxtimer::Timer t(true);
        for (const auto& sigShare : sigSharesToProcess) {
            auto quorumKey = std::make_pair(sigShare.getLlmqType(), sigShare.getQuorumHash());
            auto rs = m_shares_manager->ProcessSigShare(sigShare, quorums.at(quorumKey));
            if (rs != nullptr) {
                ProcessRecoveredSig(rs);
            }
        }
        t.stop();

        LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- processed sigShare batch. shares=%d, time=%ds\n", __func__,
                 sigSharesToProcess.size(), t.count());
    }

    return sigSharesByNodes.size() >= nMaxBatchSize;
}

void NetSigning::WorkThreadShares()
{
    int64_t lastSendTime = 0;

    assert(m_shares_manager);

    while (!workInterrupt) {
        RemoveBannedNodeStates();

        bool fMoreWork = ProcessPendingSigShares();

        {
            const std::vector<llmq::PendingSignatureData> datas = m_shares_manager->FetchPendingSigShares();
            for (const auto& pending_data : datas) {
                m_shares_manager->SignPendingSigShare(pending_data);
            }
        } // scope of data is over here to release memory

        if (TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()) - lastSendTime > 100) {
            m_shares_manager->SendMessages();
            lastSendTime = TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now());
        }

        m_shares_manager->Cleanup();

        // TODO Wakeup when pending signing is needed?
        if (!fMoreWork && !workInterrupt.sleep_for(std::chrono::milliseconds(100))) {
            return;
        }
    }
}

void NetSigning::NotifyRecoveredSig(const std::shared_ptr<const llmq::CRecoveredSig>& sig)
{
    m_peer_manager->PeerRelayRecoveredSig(Assert(sig)->GetHash());
}
