// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/net_dkg.h>

#include <active/dkgsessionhandler.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/debug.h>
#include <llmq/dkgsession.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/net_quorum.h>
#include <llmq/options.h>
#include <llmq/quorumsman.h>
#include <llmq/utils.h>
#include <masternode/meta.h>
#include <net.h>
#include <netmessagemaker.h>
#include <protocol.h>
#include <span.h>
#include <unordered_lru_cache.h>
#include <util/std23.h>
#include <util/thread.h>
#include <validation.h>

namespace llmq {

namespace {
// returns a set of NodeIds which sent invalid messages
template <typename Message>
std::unordered_set<NodeId> BatchVerifyMessageSigs(CDKGSession& session,
                                                  const std::vector<std::pair<NodeId, std::shared_ptr<Message>>>& messages)
{
    if (messages.empty()) {
        return {};
    }

    std::unordered_set<NodeId> ret;
    bool revertToSingleVerification = false;

    CBLSSignature aggSig;
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<uint256> messageHashes;
    Uint256HashSet messageHashesSet;
    pubKeys.reserve(messages.size());
    messageHashes.reserve(messages.size());
    bool first = true;
    for (const auto& [nodeId, msg] : messages) {
        auto member = session.GetMember(msg->proTxHash);
        if (!member) {
            // should not happen as it was verified before
            ret.emplace(nodeId);
            continue;
        }

        if (first) {
            aggSig = msg->sig;
        } else {
            aggSig.AggregateInsecure(msg->sig);
        }
        first = false;

        auto msgHash = msg->GetSignHash();
        if (!messageHashesSet.emplace(msgHash).second) {
            // can only happen in 2 cases:
            // 1. Someone sent us the same message twice but with differing signature, meaning that at least one of them
            //    must be invalid. In this case, we'd have to revert to single message verification nevertheless
            // 2. Someone managed to find a way to create two different binary representations of a message that deserializes
            //    to the same object representation. This would be some form of malleability. However, this shouldn't be
            //    possible as only deterministic/unique BLS signatures and very simple data types are involved
            revertToSingleVerification = true;
            break;
        }

        pubKeys.emplace_back(member->dmn->pdmnState->pubKeyOperator.Get());
        messageHashes.emplace_back(msgHash);
    }
    if (!revertToSingleVerification) {
        if (aggSig.VerifyInsecureAggregated(pubKeys, messageHashes)) {
            // all good
            return ret;
        }

        // are all messages from the same node?
        bool nodeIdsAllSame = std::adjacent_find(messages.begin(), messages.end(),
                                                 [](const auto& first, const auto& second) {
                                                     return first.first != second.first;
                                                 }) == messages.end();

        // if yes, take a short path and return a set with only him
        if (nodeIdsAllSame) {
            ret.emplace(messages[0].first);
            return ret;
        }
        // different nodes, let's figure out who are the bad ones
    }

    for (const auto& [nodeId, msg] : messages) {
        if (ret.count(nodeId)) {
            continue;
        }

        auto member = session.GetMember(msg->proTxHash);
        bool valid = msg->sig.VerifyInsecure(member->dmn->pdmnState->pubKeyOperator.Get(), msg->GetSignHash());
        if (!valid) {
            ret.emplace(nodeId);
        }
    }
    return ret;
}

void RelayInvToParticipants(const CDKGSession& session, const CConnman& connman, PeerManagerInternal& peerman,
                            const CInv& inv)
{
    CDKGLogger logger(session, __func__, __LINE__);
    std::stringstream ss;
    const auto& relayMembers = session.RelayMembers();
    for (const auto& r : relayMembers) {
        ss << r.ToString().substr(0, 4) << " | ";
    }
    logger.Batch("RelayInvToParticipants inv[%s] relayMembers[%d] GetNodeCount[%d] GetNetworkActive[%d] "
                 "HasMasternodeQuorumNodes[%d] for quorumHash[%s] forMember[%s] relayMembers[%s]",
                 inv.ToString(), relayMembers.size(), connman.GetNodeCount(ConnectionDirection::Both),
                 connman.GetNetworkActive(),
                 connman.HasMasternodeQuorumNodes(session.GetType(), session.BlockIndex()->GetBlockHash()),
                 session.BlockIndex()->GetBlockHash().ToString(), session.ProTx().ToString().substr(0, 4), ss.str());

    std::stringstream ss2;
    connman.ForEachNode([&](const CNode* pnode) {
        if (pnode->qwatch ||
            (!pnode->GetVerifiedProRegTxHash().IsNull() && (relayMembers.count(pnode->GetVerifiedProRegTxHash()) != 0))) {
            peerman.PeerPushInventory(pnode->GetId(), inv);
        }

        if (pnode->GetVerifiedProRegTxHash().IsNull()) {
            logger.Batch("node[%d:%s] not mn", pnode->GetId(), pnode->m_addr_name);
        } else if (relayMembers.count(pnode->GetVerifiedProRegTxHash()) == 0) {
            ss2 << pnode->GetVerifiedProRegTxHash().ToString().substr(0, 4) << " | ";
        }
    });
    logger.Batch("forMember[%s] NOTrelayMembers[%s]", session.ProTx().ToString().substr(0, 4), ss2.str());
    logger.Flush();
}

template <typename Message>
void EnqueueOwn(CDKGPendingMessages& pending, const Message& msg)
{
    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << msg;
    auto pm = std::make_shared<CDataStream>(std::move(ds));
    CHashWriter hw(SER_GETHASH, 0);
    hw.write(AsWritableBytes(Span{*pm}));
    pending.PushPendingMessage(/*from=*/-1, std::move(pm), hw.GetHash());
}

template <typename Message>
bool ProcessPendingMessageBatch(const CConnman& connman, CDKGSession& session, CDKGPendingMessages& pendingMessages,
                                PeerManagerInternal& peerman, size_t maxCount)
{
    auto msgs = pendingMessages.PopAndDeserializeMessages<Message>(maxCount);
    if (msgs.empty()) {
        return false;
    }

    std::vector<std::pair<NodeId, std::shared_ptr<Message>>> preverifiedMessages;
    preverifiedMessages.reserve(msgs.size());

    for (const auto& p : msgs) {
        const NodeId& nodeId = p.first;
        if (!p.second) {
            LogPrint(BCLog::LLMQ_DKG, "%s -- failed to deserialize message, peer=%d\n", __func__, nodeId);
            peerman.PeerMisbehaving(nodeId, 100);
            continue;
        }
        bool ban = false;
        if (!session.PreVerifyMessage(*p.second, ban)) {
            if (ban) {
                LogPrint(BCLog::LLMQ_DKG, "%s -- banning node due to failed preverification, peer=%d\n", __func__, nodeId);
                peerman.PeerMisbehaving(nodeId, 100);
            }
            LogPrint(BCLog::LLMQ_DKG, "%s -- skipping message due to failed preverification, peer=%d\n", __func__, nodeId);
            continue;
        }
        preverifiedMessages.emplace_back(p);
    }
    if (preverifiedMessages.empty()) {
        return true;
    }

    auto badNodes = BatchVerifyMessageSigs(session, preverifiedMessages);
    if (!badNodes.empty()) {
        for (auto nodeId : badNodes) {
            LogPrint(BCLog::LLMQ_DKG, "%s -- failed to verify signature, peer=%d\n", __func__, nodeId);
            peerman.PeerMisbehaving(nodeId, 100);
        }
    }

    for (const auto& p : preverifiedMessages) {
        const NodeId& nodeId = p.first;
        if (badNodes.count(nodeId)) {
            continue;
        }
        const std::optional<CInv> inv = session.ReceiveMessage(*p.second);
        if (inv) {
            RelayInvToParticipants(session, connman, peerman, *inv);
        }
    }

    return true;
}
} // namespace


NetDKG::NetDKG(PeerManagerInternal* peer_manager, const CSporkManager& sporkman, CDKGSessionManager& qdkgsman,
               const ChainstateManager& chainman, bool quorums_watch, CQuorumManager& qman, QuorumRole& role) :
    NetHandler(peer_manager),
    m_qdkgsman{qdkgsman},
    m_qman{qman},
    m_sporkman{sporkman},
    m_chainman{chainman},
    m_quorums_watch{quorums_watch},
    m_active{nullptr}
{
    m_qdkgsman.InitializeHandlers([](const Consensus::LLMQParams& llmq_params,
                                     [[maybe_unused]] int quorum_idx) -> std::unique_ptr<CDKGSessionHandler> {
        return std::make_unique<CDKGSessionHandler>(llmq_params);
    });
    m_qman.ConnectManagers(&role, &m_qdkgsman);
}

NetDKG::NetDKG(PeerManagerInternal* peer_manager, const CSporkManager& sporkman, CDKGSessionManager& qdkgsman,
               const ChainstateManager& chainman, bool quorums_watch, CQuorumManager& qman, QuorumRole& role,
               CBLSWorker& bls_worker, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
               CDKGDebugManager& dkgdbgman, CQuorumBlockProcessor& qblockman, CQuorumSnapshotManager& qsnapman,
               const CActiveMasternodeManager& mn_activeman, CConnman& connman) :
    NetHandler(peer_manager),
    m_qdkgsman{qdkgsman},
    m_qman{qman},
    m_sporkman{sporkman},
    m_chainman{chainman},
    m_quorums_watch{quorums_watch},
    m_active{std::make_unique<ActiveDKG>(ActiveDKG{dmnman, mn_metaman, dkgdbgman, qblockman, qsnapman, connman})}
{
    m_qdkgsman.InitializeHandlers(
        [&](const Consensus::LLMQParams& llmq_params, int quorum_idx) -> std::unique_ptr<ActiveDKGSessionHandler> {
            return std::make_unique<ActiveDKGSessionHandler>(bls_worker, dmnman, mn_metaman, dkgdbgman, qdkgsman,
                                                             qblockman, qsnapman, mn_activeman, chainman, sporkman,
                                                             llmq_params, quorums_watch, quorum_idx);
        });
    m_qman.ConnectManagers(&role, &m_qdkgsman);
}

NetDKG::~NetDKG()
{
    Stop();
    m_qman.DisconnectManagers();
}

void NetDKG::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    if (!IsQuorumDKGEnabled(m_sporkman)) return;

    if (msg_type != NetMsgType::QCONTRIB && msg_type != NetMsgType::QCOMPLAINT && msg_type != NetMsgType::QJUSTIFICATION &&
        msg_type != NetMsgType::QPCOMMITMENT && msg_type != NetMsgType::QWATCH) {
        return;
    }

    const bool is_masternode = m_active != nullptr;

    if (msg_type == NetMsgType::QWATCH) {
        if (!is_masternode) {
            // non-masternodes should never receive this
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10);
            return;
        }
        pfrom.qwatch = true;
        return;
    }

    if (vRecv.empty()) {
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), 100);
        return;
    }

    Consensus::LLMQType llmqType;
    uint256 quorumHash;
    vRecv >> llmqType;
    vRecv >> quorumHash;
    vRecv.Rewind(sizeof(uint256));
    vRecv.Rewind(sizeof(uint8_t));

    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    if (!llmq_params_opt.has_value()) {
        LogPrintf("NetDKG -- invalid llmqType [%d]\n", std23::to_underlying(llmqType));
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), 100);
        return;
    }
    const auto& llmq_params = llmq_params_opt.value();

    int quorumIndex{-1};
    {
        LOCK(cs_indexed_quorums_cache);
        if (indexed_quorums_cache.empty()) {
            utils::InitQuorumsCache(indexed_quorums_cache, m_chainman.GetConsensus());
        }
        indexed_quorums_cache[llmqType].get(quorumHash, quorumIndex);
    }

    if (quorumIndex == -1) {
        const CBlockIndex* pQuorumBaseBlockIndex = WITH_LOCK(::cs_main,
                                                             return m_chainman.m_blockman.LookupBlockIndex(quorumHash));
        if (pQuorumBaseBlockIndex == nullptr) {
            LogPrintf("NetDKG -- unknown quorumHash %s\n", quorumHash.ToString());
            // NOTE: do not insta-ban for this, we might be lagging behind
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10);
            return;
        }
        if (!m_chainman.IsQuorumTypeEnabled(llmqType, pQuorumBaseBlockIndex->pprev)) {
            LogPrintf("NetDKG -- llmqType [%d] quorums aren't active\n", std23::to_underlying(llmqType));
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 100);
            return;
        }
        quorumIndex = pQuorumBaseBlockIndex->nHeight % llmq_params.dkgInterval;
        const int quorumIndexMax = IsQuorumRotationEnabled(llmq_params, pQuorumBaseBlockIndex)
                                       ? llmq_params.signingActiveQuorumCount - 1
                                       : 0;
        if (quorumIndex > quorumIndexMax) {
            LogPrintf("NetDKG -- invalid quorumHash %s\n", quorumHash.ToString());
            m_peer_manager->PeerMisbehaving(pfrom.GetId(), 100);
            return;
        }
    }

    int inv_type = 0;
    if (msg_type == NetMsgType::QCONTRIB)
        inv_type = MSG_QUORUM_CONTRIB;
    else if (msg_type == NetMsgType::QCOMPLAINT)
        inv_type = MSG_QUORUM_COMPLAINT;
    else if (msg_type == NetMsgType::QJUSTIFICATION)
        inv_type = MSG_QUORUM_JUSTIFICATION;
    else if (msg_type == NetMsgType::QPCOMMITMENT)
        inv_type = MSG_QUORUM_PREMATURE_COMMITMENT;
    Assume(inv_type != 0); // guarded by the early-return above

    auto pm = std::make_shared<CDataStream>(std::move(vRecv));
    CHashWriter hw(SER_GETHASH, 0);
    hw.write(AsWritableBytes(Span{*pm}));
    const uint256 hash = hw.GetHash();

    const NodeId from = pfrom.GetId();
    const bool dispatched = m_qdkgsman.DoForHandler({llmqType, quorumIndex}, [&](CDKGSessionHandler& handler) {
        CDKGPendingMessages* pending = nullptr;
        switch (inv_type) {
        case MSG_QUORUM_CONTRIB:
            pending = &handler.pendingContributions;
            break;
        case MSG_QUORUM_COMPLAINT:
            pending = &handler.pendingComplaints;
            break;
        case MSG_QUORUM_JUSTIFICATION:
            pending = &handler.pendingJustifications;
            break;
        case MSG_QUORUM_PREMATURE_COMMITMENT:
            pending = &handler.pendingPrematureCommitments;
            break;
        }
        Assume(pending != nullptr);
        WITH_LOCK(::cs_main, m_peer_manager->PeerEraseObjectRequest(from, CInv{static_cast<uint32_t>(inv_type), hash}));
        pending->PushPendingMessage(from, std::move(pm), hash);
    });
    if (!dispatched) {
        LogPrintf("NetDKG -- no session handlers for quorumIndex [%d]\n", quorumIndex);
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), 100);
        return;
    }

    WITH_LOCK(cs_indexed_quorums_cache, indexed_quorums_cache[llmqType].insert(quorumHash, quorumIndex));
}

bool NetDKG::AlreadyHave(const CInv& inv)
{
    switch (inv.type) {
    case MSG_QUORUM_CONTRIB:
    case MSG_QUORUM_COMPLAINT:
    case MSG_QUORUM_JUSTIFICATION:
    case MSG_QUORUM_PREMATURE_COMMITMENT: {
        if (!IsQuorumDKGEnabled(m_sporkman)) return false;
        bool seen = false;
        m_qdkgsman.ForEachHandler([&](CDKGSessionHandler& h) {
            if (seen) return;
            if (h.pendingContributions.HasSeen(inv.hash) || h.pendingComplaints.HasSeen(inv.hash) ||
                h.pendingJustifications.HasSeen(inv.hash) || h.pendingPrematureCommitments.HasSeen(inv.hash)) {
                seen = true;
            }
        });
        return seen;
    }
    }
    return false;
}

bool NetDKG::ProcessGetData(CNode& pfrom, const CInv& inv, CConnman& connman, const CNetMsgMaker& msgMaker)
{
    // Default implementations of GetContribution and the other virtual methods
    // return false in observer mode; m_active is only an early exit and does
    // not affect logic.
    if (m_active == nullptr) return false;

    switch (inv.type) {
    case MSG_QUORUM_CONTRIB: {
        CDKGContribution o;
        if (m_qdkgsman.GetContribution(inv.hash, o)) {
            connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::QCONTRIB, o));
            return true;
        }
        return false;
    }
    case MSG_QUORUM_COMPLAINT: {
        CDKGComplaint o;
        if (m_qdkgsman.GetComplaint(inv.hash, o)) {
            connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::QCOMPLAINT, o));
            return true;
        }
        return false;
    }
    case MSG_QUORUM_JUSTIFICATION: {
        CDKGJustification o;
        if (m_qdkgsman.GetJustification(inv.hash, o)) {
            connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::QJUSTIFICATION, o));
            return true;
        }
        return false;
    }
    case MSG_QUORUM_PREMATURE_COMMITMENT: {
        CDKGPrematureCommitment o;
        if (m_qdkgsman.GetPrematureCommitment(inv.hash, o)) {
            connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::QPCOMMITMENT, o));
            return true;
        }
        return false;
    }
    }
    return false;
}

void NetDKG::Start()
{
    if (m_active == nullptr) return;
    if (!m_phase_threads.empty()) {
        throw std::runtime_error("Tried to start PhaseHandlerThreads again.");
    }

    m_qdkgsman.ForEachHandler([this](CDKGSessionHandler& base) {
        auto& handler = dynamic_cast<ActiveDKGSessionHandler&>(base);
        std::string thread_name = strprintf("llmq-%d-%d", std23::to_underlying(handler.params.type), handler.QuorumIndex());
        m_phase_threads.emplace_back([this, name = std::move(thread_name), &handler] {
            util::TraceThread(name.c_str(), [this, &handler] { PhaseHandlerThread(handler); });
        });
    });
}

void NetDKG::Stop()
{
    Interrupt();
    for (auto& t : m_phase_threads) {
        if (t.joinable()) t.join();
    }
    m_phase_threads.clear();
}

void NetDKG::Interrupt()
{
    if (m_active == nullptr) return;
    m_qdkgsman.ForEachHandler([](CDKGSessionHandler& base) {
        if (auto* handler = dynamic_cast<ActiveDKGSessionHandler*>(&base)) {
            handler->RequestStop();
        }
    });
}

void NetDKG::PhaseHandlerThread(ActiveDKGSessionHandler& handler)
{
    while (!handler.IsStopRequested()) {
        try {
            LogPrint(BCLog::LLMQ_DKG, "NetDKG::%s -- %s qi[%d] - starting HandleDKGRound\n", __func__,
                     handler.params.name, handler.QuorumIndex());
            HandleDKGRound(handler);
        } catch (AbortPhaseException& e) {
            m_active->dkgdbgman.MarkAborted(handler.params.type, handler.QuorumIndex());
            LogPrint(BCLog::LLMQ_DKG, "NetDKG::%s -- %s qi[%d] - aborted current DKG session\n", __func__,
                     handler.params.name, handler.QuorumIndex());
        }
    }
}

static void AddQuorumProbeConnections(const Consensus::LLMQParams& llmqParams, CConnman& connman,
                                      CMasternodeMetaMan& mn_metaman, const CSporkManager& sporkman,
                                      const UtilParameters& util_params, const CDeterministicMNList& tip_mn_list,
                                      const uint256& myProTxHash)
{
    assert(mn_metaman.IsValid());

    if (!IsQuorumPoseEnabled(llmqParams.type, sporkman)) {
        return;
    }

    auto members = utils::GetAllQuorumMembers(llmqParams.type, util_params);
    auto curTime = GetTime<std::chrono::seconds>().count();

    Uint256HashSet probeConnections;
    for (const auto& dmn : members) {
        if (dmn->proTxHash == myProTxHash) {
            continue;
        }
        auto lastOutbound = mn_metaman.GetLastOutboundSuccess(dmn->proTxHash);
        if (curTime - lastOutbound < 10 * 60) {
            // avoid re-probing nodes too often
            continue;
        }
        probeConnections.emplace(dmn->proTxHash);
    }

    if (!probeConnections.empty()) {
        if (LogAcceptDebug(BCLog::LLMQ)) {
            std::string debugMsg = strprintf("%s -- adding masternodes probes for quorum %s:\n", __func__,
                                             util_params.m_base_index->GetBlockHash().ToString());
            for (const auto& c : probeConnections) {
                auto dmn = tip_mn_list.GetValidMN(c);
                if (!dmn) {
                    debugMsg += strprintf("  %s (not in valid MN set anymore)\n", c.ToString());
                } else {
                    debugMsg += strprintf("  %s (%s)\n", c.ToString(),
                                          dmn->pdmnState->netInfo->GetPrimary().ToStringAddrPort());
                }
            }
            LogPrint(BCLog::NET_NETCONN, debugMsg.c_str()); /* Continued */
        }
        connman.AddPendingProbeConnections(probeConnections);
    }
}

void NetDKG::HandleDKGRound(ActiveDKGSessionHandler& handler)
{
    auto& active = *Assert(m_active);

    handler.WaitForNextPhase(std::nullopt, QuorumPhase::Initialized);

    handler.ClearPendingMessages();
    uint256 curQuorumHash = handler.GetCurrentQuorumHash();

    const CBlockIndex* pQuorumBaseBlockIndex = WITH_LOCK(::cs_main,
                                                         return m_chainman.m_blockman.LookupBlockIndex(curQuorumHash));

    if (!pQuorumBaseBlockIndex || !handler.InitNewQuorum(pQuorumBaseBlockIndex)) {
        // should actually never happen
        handler.WaitForNewQuorum(curQuorumHash);
        throw AbortPhaseException();
    }

    active.dkgdbgman.MarkPhaseAdvanced(handler.params.type, handler.QuorumIndex(), QuorumPhase::Initialized);

    auto* curSession = handler.GetCurSession();
    if (handler.params.is_single_member()) {
        auto finalCommitment = curSession->FinalizeSingleCommitment();
        if (!finalCommitment.IsNull()) { // it can be null only if we are not member
            if (auto inv_opt = active.qblockman.AddMineableCommitment(finalCommitment); inv_opt.has_value()) {
                m_peer_manager->PeerRelayInv(inv_opt.value());
            }
        }
        handler.WaitForNextPhase(QuorumPhase::Initialized, QuorumPhase::Contribute, curQuorumHash);
        return;
    }

    const auto tip_mn_list = active.dmnman.GetListAtChainTip();
    llmq::EnsureQuorumConnections(handler.params, active.connman, m_sporkman,
                                  {active.dmnman, active.qsnapman, m_chainman, pQuorumBaseBlockIndex}, tip_mn_list,
                                  curSession->ProTx(), /*is_masternode=*/true, handler.QuorumsWatch());
    if (curSession->AreWeMember()) {
        AddQuorumProbeConnections(handler.params, active.connman, active.mn_metaman, m_sporkman,
                                  {active.dmnman, active.qsnapman, m_chainman, pQuorumBaseBlockIndex}, tip_mn_list,
                                  curSession->ProTx());
    }

    handler.WaitForNextPhase(QuorumPhase::Initialized, QuorumPhase::Contribute, curQuorumHash);

    // Contribute
    auto fContributeStart = [curSession, &handler]() {
        if (auto qc = curSession->Contribute(); qc) {
            EnqueueOwn(handler.pendingContributions, *qc);
        }
    };
    auto fContributeWait = [this, curSession, &handler, &active] {
        return ProcessPendingMessageBatch<CDKGContribution>(active.connman, *curSession, handler.pendingContributions,
                                                            *m_peer_manager, 8);
    };
    handler.HandlePhase(QuorumPhase::Contribute, QuorumPhase::Complain, curQuorumHash, 0.05, fContributeStart,
                        fContributeWait);

    // Complain
    auto fComplainStart = [curSession, &handler, &active]() {
        if (auto qc = curSession->VerifyAndComplain(active.connman); qc) {
            EnqueueOwn(handler.pendingComplaints, *qc);
        }
    };
    auto fComplainWait = [this, curSession, &handler, &active] {
        return ProcessPendingMessageBatch<CDKGComplaint>(active.connman, *curSession, handler.pendingComplaints,
                                                         *m_peer_manager, 8);
    };
    handler.HandlePhase(QuorumPhase::Complain, QuorumPhase::Justify, curQuorumHash, 0.05, fComplainStart, fComplainWait);

    // Justify
    auto fJustifyStart = [curSession, &handler]() {
        if (auto qj = curSession->VerifyAndJustify(); qj) {
            EnqueueOwn(handler.pendingJustifications, *qj);
        }
    };
    auto fJustifyWait = [this, curSession, &handler, &active] {
        return ProcessPendingMessageBatch<CDKGJustification>(active.connman, *curSession, handler.pendingJustifications,
                                                             *m_peer_manager, 8);
    };
    handler.HandlePhase(QuorumPhase::Justify, QuorumPhase::Commit, curQuorumHash, 0.05, fJustifyStart, fJustifyWait);

    // Commit
    auto fCommitStart = [curSession, &handler]() {
        if (auto qc = curSession->VerifyAndCommit(); qc) {
            EnqueueOwn(handler.pendingPrematureCommitments, *qc);
        }
    };
    auto fCommitWait = [this, curSession, &handler, &active] {
        return ProcessPendingMessageBatch<CDKGPrematureCommitment>(active.connman, *curSession,
                                                                   handler.pendingPrematureCommitments, *m_peer_manager,
                                                                   8);
    };
    handler.HandlePhase(QuorumPhase::Commit, QuorumPhase::Finalize, curQuorumHash, 0.1, fCommitStart, fCommitWait);

    auto finalCommitments = curSession->FinalizeCommitments();
    for (const auto& fqc : finalCommitments) {
        if (auto inv_opt = active.qblockman.AddMineableCommitment(fqc); inv_opt.has_value()) {
            m_peer_manager->PeerRelayInv(inv_opt.value());
        }
    }
}

void NetDKGStub::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    if (msg_type == NetMsgType::QCONTRIB || msg_type == NetMsgType::QCOMPLAINT || msg_type == NetMsgType::QJUSTIFICATION ||
        msg_type == NetMsgType::QPCOMMITMENT || msg_type == NetMsgType::QWATCH) {
        m_peer_manager->PeerMisbehaving(pfrom.GetId(), 10);
    }
}

} // namespace llmq
