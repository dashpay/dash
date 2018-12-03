// Copyright (c) 2018 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "quorums_dkgsessionmgr.h"

#include "quorums.h"
#include "quorums_blockprocessor.h"
#include "quorums_debug.h"
#include "quorums_utils.h"

#include "evo/specialtx.h"

#include "activemasternode.h"
#include "validation.h"
#include "netmessagemaker.h"
#include "univalue.h"
#include "chainparams.h"
#include "spork.h"
#include "net_processing.h"

#include "init.h"

#include "cxxtimer.hpp"

namespace llmq
{

CDKGSessionManager* quorumDKGSessionManager;

static const std::string DB_VVEC = "qdkg_V";
static const std::string DB_SKCONTRIB = "qdkg_S";

CDKGPendingMessages::CDKGPendingMessages(size_t _maxMessagesPerNode) :
    maxMessagesPerNode(_maxMessagesPerNode)
{
}

void CDKGPendingMessages::PushPendingMessage(NodeId from, CDataStream& vRecv)
{
    LOCK(cs);

    // this will also consume the data, even if we bail out early
    auto pm = std::make_shared<CDataStream>(std::move(vRecv));

    if (messagesPerNode[from] >= maxMessagesPerNode) {
        // TODO ban?
        LogPrint("net", "CDKGPendingMessages::%s -- too many messages, peer=%d\n", __func__, from);
        return;
    }
    messagesPerNode[from]++;

    CHashWriter hw(SER_GETHASH, 0);
    hw.write(pm->data(), pm->size());
    uint256 hash = hw.GetHash();

    if (!seenMessages.emplace(hash).second) {
        LogPrint("net", "CDKGPendingMessages::%s -- already seen %s, peer=%d", __func__, from);
        return;
    }

    {
        LOCK(cs_main);
        g_connman->RemoveAskFor(hash);
    }

    pendingMessages.emplace_back(std::make_pair(from, std::move(pm)));
}

std::list<CDKGPendingMessages::BinaryMessage> CDKGPendingMessages::PopPendingMessages(size_t maxCount)
{
    LOCK(cs);

    std::list<BinaryMessage> ret;
    while (!pendingMessages.empty() && ret.size() < maxCount) {
        ret.emplace_back(std::move(pendingMessages.front()));
        pendingMessages.pop_front();
    }

    return std::move(ret);
}

bool CDKGPendingMessages::HasSeen(const uint256& hash) const
{
    LOCK(cs);
    return seenMessages.count(hash) != 0;
}

void CDKGPendingMessages::Clear()
{
    LOCK(cs);
    pendingMessages.clear();
    messagesPerNode.clear();
    seenMessages.clear();
}

//////

CDKGSessionHandler::CDKGSessionHandler(const Consensus::LLMQParams& _params, CEvoDB& _evoDb, ctpl::thread_pool& _messageHandlerPool, CBLSWorker& _blsWorker, CDKGSessionManager& _dkgManager) :
    params(_params),
    evoDb(_evoDb),
    messageHandlerPool(_messageHandlerPool),
    blsWorker(_blsWorker),
    dkgManager(_dkgManager),
    curSession(std::make_shared<CDKGSession>(_params, _evoDb, _blsWorker, _dkgManager)),
    pendingContributions((size_t)_params.size * 2), // we allow size*2 messages as we need to make sure we see bad behavior (double messages)
    pendingComplaints((size_t)_params.size * 2),
    pendingJustifications((size_t)_params.size * 2),
    pendingPrematureCommitments((size_t)_params.size * 2)
{
    phaseHandlerThread = std::thread([this] {
        RenameThread(strprintf("quorum-phase-%d", (uint8_t)params.type).c_str());
        PhaseHandlerThread();
    });
}

CDKGSessionHandler::~CDKGSessionHandler()
{
    if (phaseHandlerThread.joinable()) {
        phaseHandlerThread.join();
    }
}

void CDKGSessionHandler::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    AssertLockHeld(cs_main);
    LOCK(cs);

    int quorumStageInt = pindexNew->nHeight % params.dkgInterval;
    CBlockIndex* pindexQuorum = chainActive[pindexNew->nHeight - quorumStageInt];

    quorumHeight = pindexQuorum->nHeight;
    quorumHash = pindexQuorum->GetBlockHash();

    QuorumPhase newPhase = phase;
    if (quorumStageInt == 0) {
        newPhase = QuorumPhase_Initialized;
        quorumDKGDebugManager->ResetLocalSessionStatus(params.type, quorumHash, quorumHeight);
    } else if (quorumStageInt == params.dkgPhaseBlocks * 1) {
        newPhase = QuorumPhase_Contribute;
    } else if (quorumStageInt == params.dkgPhaseBlocks * 2) {
        newPhase = QuorumPhase_Complain;
    } else if (quorumStageInt == params.dkgPhaseBlocks * 3) {
        newPhase = QuorumPhase_Justify;
    } else if (quorumStageInt == params.dkgPhaseBlocks * 4) {
        newPhase = QuorumPhase_Commit;
    } else if (quorumStageInt == params.dkgPhaseBlocks * 5) {
        newPhase = QuorumPhase_Finalize;
    } else if (quorumStageInt == params.dkgPhaseBlocks * 6) {
        newPhase = QuorumPhase_Idle;
    }
    phase = newPhase;

    quorumDKGDebugManager->UpdateLocalStatus([&](CDKGDebugStatus& status) {
        bool changed = status.nHeight != pindexNew->nHeight;
        status.nHeight = (uint32_t)pindexNew->nHeight;
        return changed;
    });
    quorumDKGDebugManager->UpdateLocalSessionStatus(params.type, [&](CDKGDebugSessionStatus& status) {
        bool changed = status.phase != (uint8_t)phase;
        status.phase = (uint8_t)phase;
        return changed;
    });
}

void CDKGSessionHandler::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    // We don't handle messages in the calling thread as deserialization/processing of these would block everything
    if (strCommand == NetMsgType::QCONTRIB) {
        pendingContributions.PushPendingMessage(pfrom->id, vRecv);
    } else if (strCommand == NetMsgType::QCOMPLAINT) {
        pendingComplaints.PushPendingMessage(pfrom->id, vRecv);
    } else if (strCommand == NetMsgType::QJUSTIFICATION) {
        pendingJustifications.PushPendingMessage(pfrom->id, vRecv);
    } else if (strCommand == NetMsgType::QPCOMMITMENT) {
        pendingPrematureCommitments.PushPendingMessage(pfrom->id, vRecv);
    }
}

bool CDKGSessionHandler::InitNewQuorum(int height, const uint256& quorumHash)
{
    //AssertLockHeld(cs_main);

    const auto& consensus = Params().GetConsensus();

    curSession = std::make_shared<CDKGSession>(params, evoDb, blsWorker, dkgManager);

    if (!deterministicMNManager->IsDeterministicMNsSporkActive(height)) {
        return false;
    }

    auto mns = CLLMQUtils::GetAllQuorumMembers(params.type, quorumHash);

    if (!curSession->Init(height, quorumHash, mns, activeMasternodeInfo.proTxHash)) {
        LogPrintf("CDKGSessionManager::%s -- quorum initialiation failed\n", __func__);
        return false;
    }

    return true;
}

std::pair<QuorumPhase, uint256> CDKGSessionHandler::GetPhaseAndQuorumHash()
{
    LOCK(cs);
    return std::make_pair(phase, quorumHash);
}

class AbortPhaseException : public std::exception {
};

void CDKGSessionHandler::WaitForNextPhase(QuorumPhase curPhase,
                                          QuorumPhase nextPhase,
                                          uint256& expectedQuorumHash,
                                          const WhileWaitFunc& runWhileWaiting)
{
    while (true) {
        if (ShutdownRequested()) {
            throw AbortPhaseException();
        }
        auto p = GetPhaseAndQuorumHash();
        if (!expectedQuorumHash.IsNull() && p.second != expectedQuorumHash) {
            throw AbortPhaseException();
        }
        if (p.first == nextPhase) {
            expectedQuorumHash = p.second;
            return;
        }
        if (curPhase != QuorumPhase_None && p.first != curPhase) {
            throw AbortPhaseException();
        }
        if (!runWhileWaiting()) {
            MilliSleep(100);
        }
    }
}

void CDKGSessionHandler::WaitForNewQuorum(const uint256& oldQuorumHash)
{
    while (true) {
        if (ShutdownRequested()) {
            throw AbortPhaseException();
        }
        auto p = GetPhaseAndQuorumHash();
        if (p.second != oldQuorumHash) {
            return;
        }
        MilliSleep(100);
    }
}

void CDKGSessionHandler::RandomSleep(QuorumPhase curPhase,
                                     uint256& expectedQuorumHash,
                                     double randomSleepFactor,
                                     const WhileWaitFunc& runWhileWaiting)
{
    // Randomly sleep some time to not fully overload the whole network
    int64_t endTime = GetTimeMillis() + GetRandInt((int)(params.dkgRndSleepTime * randomSleepFactor));
    while (GetTimeMillis() < endTime) {
        if (ShutdownRequested()) {
            throw AbortPhaseException();
        }
        auto p = GetPhaseAndQuorumHash();
        if (p.first != curPhase || p.second != expectedQuorumHash) {
            throw AbortPhaseException();
        }
        if (!runWhileWaiting()) {
            MilliSleep(100);
        }
    }
}

void CDKGSessionHandler::HandlePhase(QuorumPhase curPhase,
                                     QuorumPhase nextPhase,
                                     uint256& expectedQuorumHash,
                                     double randomSleepFactor,
                                     const StartPhaseFunc& startPhaseFunc,
                                     const WhileWaitFunc& runWhileWaiting)
{
    RandomSleep(curPhase, expectedQuorumHash, randomSleepFactor, runWhileWaiting);
    startPhaseFunc();
    WaitForNextPhase(curPhase, nextPhase, expectedQuorumHash, runWhileWaiting);
}

// returns a set of NodeIds which sent invalid messages
template<typename Message>
std::set<NodeId> BatchVerifyMessageSigs(CDKGSession& session, const std::vector<std::pair<NodeId, std::shared_ptr<Message>>>& messages)
{
    if (messages.empty()) {
        return {};
    }

    std::set<NodeId> ret;
    bool revertToSingleVerification = false;

    CBLSSignature aggSig;
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<uint256> messageHashes;
    std::set<uint256> messageHashesSet;
    pubKeys.reserve(messages.size());
    messageHashes.reserve(messages.size());
    bool first = true;
    for (const auto& p : messages ) {
        const auto& msg = *p.second;

        auto member = session.GetMember(msg.proTxHash);
        if (!member) {
            // should not happen as it was verified before
            ret.emplace(p.first);
            continue;
        }

        if (first) {
            aggSig = msg.sig;
        } else {
            aggSig.AggregateInsecure(msg.sig);
        }
        first = false;

        auto msgHash = msg.GetSignHash();
        if (!messageHashesSet.emplace(msgHash).second) {
            // can only happen in 2 cases:
            // 1. Someone sent us the same message twice but with differing signature, meaning that at least one of them
            //    must be invalid. In this case, we'd have to revert to single message verification nevertheless
            // 2. Someone managed to find a way to create two different binary representations of a message that deserializes
            //    to the same object representation. This would be some form of malleability. However, this shouldn't
            //    possible as only deterministic/unique BLS signatures and very simple data types are involved
            revertToSingleVerification = true;
            break;
        }

        pubKeys.emplace_back(member->dmn->pdmnState->pubKeyOperator);
        messageHashes.emplace_back(msgHash);
    }
    if (!revertToSingleVerification) {
        bool valid = aggSig.VerifyInsecureAggregated(pubKeys, messageHashes);
        if (valid) {
            // all good
            return ret;
        }

        // are all messages from the same node?
        NodeId firstNodeId;
        first = true;
        bool nodeIdsAllSame = true;
        for (auto it = messages.begin(); it != messages.end(); ++it) {
            if (first) {
                firstNodeId = it->first;
            } else {
                first = false;
                if (it->first != firstNodeId) {
                    nodeIdsAllSame = false;
                    break;
                }
            }
        }
        // if yes, take a short path and return a set with only him
        if (nodeIdsAllSame) {
            ret.emplace(firstNodeId);
            return ret;
        }
        // different nodes, let's figure out who are the bad ones
    }

    for (const auto& p : messages) {
        if (ret.count(p.first)) {
            continue;
        }

        const auto& msg = *p.second;
        auto member = session.GetMember(msg.proTxHash);
        bool valid = msg.sig.VerifyInsecure(member->dmn->pdmnState->pubKeyOperator, msg.GetSignHash());
        if (!valid) {
            ret.emplace(p.first);
        }
    }
    return ret;
}

template<typename Message>
bool ProcessPendingMessageBatch(CDKGSession& session, CDKGPendingMessages& pendingMessages, size_t maxCount)
{
    auto msgs = pendingMessages.PopAndDeserializeMessages<Message>(maxCount);
    if (msgs.empty()) {
        return false;
    }

    std::vector<uint256> hashes;
    std::vector<std::pair<NodeId, std::shared_ptr<Message>>> preverifiedMessages;
    hashes.reserve(msgs.size());
    preverifiedMessages.reserve(msgs.size());

    for (const auto& p : msgs) {
        if (!p.second) {
            LogPrint("net", "%s -- failed to deserialize message, peer=%d", __func__, p.first);
            {
                LOCK(cs_main);
                Misbehaving(p.first, 100);
            }
            continue;
        }
        const auto& msg = *p.second;

        auto hash = ::SerializeHash(msg);
        {
            LOCK(cs_main);
            g_connman->RemoveAskFor(hash);
        }

        bool ban = false;
        if (!session.PreVerifyMessage(hash, msg, ban)) {
            if (ban) {
                LogPrint("net", "%s -- banning node due to failed preverification, peer=%d", __func__, p.first);
                {
                    LOCK(cs_main);
                    Misbehaving(p.first, 100);
                }
            }
            LogPrint("net", "%s -- skipping message due to failed preverification, peer=%d", __func__, p.first);
            continue;
        }
        hashes.emplace_back(hash);
        preverifiedMessages.emplace_back(p);
    }
    if (preverifiedMessages.empty()) {
        return true;
    }

    auto badNodes = BatchVerifyMessageSigs(session, preverifiedMessages);
    if (!badNodes.empty()) {
        LOCK(cs_main);
        for (auto nodeId : badNodes) {
            LogPrint("net", "%s -- failed to deserialize message, peer=%d", __func__, nodeId);
            Misbehaving(nodeId, 100);
        }
    }

    for (size_t i = 0; i < preverifiedMessages.size(); i++) {
        NodeId nodeId = preverifiedMessages[i].first;
        if (badNodes.count(nodeId)) {
            continue;
        }
        const auto& msg = *preverifiedMessages[i].second;
        bool ban = false;
        session.ReceiveMessage(hashes[i], msg, ban);
        if (ban) {
            LogPrint("net", "%s -- banning node after ReceiveMessage failed, peer=%d", __func__, nodeId);
            LOCK(cs_main);
            Misbehaving(nodeId, 100);
            badNodes.emplace(nodeId);
        }
    }

    for (const auto& p : preverifiedMessages) {
        NodeId nodeId = p.first;
        if (badNodes.count(nodeId)) {
            continue;
        }
        session.AddParticipatingNode(nodeId);
    }

    return true;
}

void CDKGSessionHandler::HandleDKGRound()
{
    uint256 curQuorumHash;

    WaitForNextPhase(QuorumPhase_None, QuorumPhase_Initialized, curQuorumHash, []{return false;});

    {
        LOCK(cs);
        pendingContributions.Clear();
        pendingComplaints.Clear();
        pendingJustifications.Clear();
        pendingPrematureCommitments.Clear();
    }

    if (!InitNewQuorum(quorumHeight, quorumHash)) {
        // should actually never happen
        WaitForNewQuorum(curQuorumHash);
        throw AbortPhaseException();
    }

    if (curSession->AreWeMember() || GetBoolArg("-watchquorums", DEFAULT_WATCH_QUORUMS)) {
        std::set<CService> connections;
        if (curSession->AreWeMember()) {
            connections = CLLMQUtils::GetQuorumConnections(params.type, curQuorumHash, curSession->myProTxHash);
        } else {
            auto cindexes = CLLMQUtils::CalcDeterministicWatchConnections(params.type, curQuorumHash, curSession->members.size(), 1);
            for (auto idx : cindexes) {
                connections.emplace(curSession->members[idx]->dmn->pdmnState->addr);
            }
        }
        if (!connections.empty()) {
            std::string debugMsg = strprintf("CDKGSessionManager::%s -- adding masternodes quorum connections for quorum %s:\n", __func__, curSession->quorumHash.ToString());
            for (auto& c : connections) {
                debugMsg += strprintf("  %s\n", c.ToString(false));
            }
            LogPrintf(debugMsg);
            g_connman->AddMasternodeQuorumNodes(params.type, curQuorumHash, connections);

            LOCK(curSession->invCs);
            curSession->participatingNodes = g_connman->GetMasternodeQuorumAddresses(params.type, curQuorumHash);
        }
    }

    WaitForNextPhase(QuorumPhase_Initialized, QuorumPhase_Contribute, curQuorumHash, []{return false;});

    // Contribute
    auto fContributeStart = [this]() {
        curSession->Contribute();
    };
    auto fContributeWait = [this] {
        return ProcessPendingMessageBatch<CDKGContribution>(*curSession, pendingContributions, 8);
    };
    HandlePhase(QuorumPhase_Contribute, QuorumPhase_Complain, curQuorumHash, 1, fContributeStart, fContributeWait);

    // Complain
    auto fComplainStart = [this]() {
        curSession->VerifyAndComplain();
    };
    auto fComplainWait = [this] {
        return ProcessPendingMessageBatch<CDKGComplaint>(*curSession, pendingComplaints, 8);
    };
    HandlePhase(QuorumPhase_Complain, QuorumPhase_Justify, curQuorumHash, 0, fComplainStart, fComplainWait);

    // Justify
    auto fJustifyStart = [this]() {
        curSession->VerifyAndJustify();
    };
    auto fJustifyWait = [this] {
        return ProcessPendingMessageBatch<CDKGJustification>(*curSession, pendingJustifications, 8);
    };
    HandlePhase(QuorumPhase_Justify, QuorumPhase_Commit, curQuorumHash, 0, fJustifyStart, fJustifyWait);

    // Commit
    auto fCommitStart = [this]() {
        curSession->VerifyAndCommit();
    };
    auto fCommitWait = [this] {
        return ProcessPendingMessageBatch<CDKGPrematureCommitment>(*curSession, pendingPrematureCommitments, 8);
    };
    HandlePhase(QuorumPhase_Commit, QuorumPhase_Finalize, curQuorumHash, 1, fCommitStart, fCommitWait);

    auto finalCommitments = curSession->FinalizeCommitments();
    for (auto& fqc : finalCommitments) {
        quorumBlockProcessor->AddMinableCommitment(fqc);
    }
}

void CDKGSessionHandler::PhaseHandlerThread()
{
    while (!ShutdownRequested()) {
        try {
            HandleDKGRound();
        } catch (AbortPhaseException& e) {
            quorumDKGDebugManager->UpdateLocalSessionStatus(params.type, [&](CDKGDebugSessionStatus& status) {
                status.aborted = true;
                return true;
            });
            LogPrintf("CDKGSessionHandler::%s -- aborted current DKG session\n", __func__);
        }
    }
}

////

CDKGSessionManager::CDKGSessionManager(CEvoDB& _evoDb, CBLSWorker& _blsWorker) :
    evoDb(_evoDb),
    blsWorker(_blsWorker)
{
    for (auto& qt : Params().GetConsensus().llmqs) {
        dkgSessionHandlers.emplace(std::piecewise_construct,
                std::forward_as_tuple(qt.first),
                std::forward_as_tuple(qt.second, _evoDb, messageHandlerPool, blsWorker, *this));
    }

    messageHandlerPool.resize(2);
    RenameThreadPool(messageHandlerPool, "quorum-msg");
}

CDKGSessionManager::~CDKGSessionManager()
{
    messageHandlerPool.stop(true);
}

void CDKGSessionManager::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    const auto& consensus = Params().GetConsensus();

    if (fInitialDownload)
        return;
    if (!deterministicMNManager->IsDeterministicMNsSporkActive(pindexNew->nHeight))
        return;
    if (!sporkManager.IsSporkActive(SPORK_17_QUORUM_DKG_ENABLED))
        return;

    LOCK(cs_main);

    for (auto& qt : dkgSessionHandlers) {
        qt.second.UpdatedBlockTip(pindexNew, pindexFork, fInitialDownload);
    }
}

void CDKGSessionManager::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if (!sporkManager.IsSporkActive(SPORK_17_QUORUM_DKG_ENABLED))
        return;

    if (strCommand != NetMsgType::QCONTRIB
        && strCommand != NetMsgType::QCOMPLAINT
        && strCommand != NetMsgType::QJUSTIFICATION
        && strCommand != NetMsgType::QPCOMMITMENT
        && strCommand != NetMsgType::QWATCH) {
        return;
    }

    if (strCommand == NetMsgType::QWATCH) {
        pfrom->qwatch = true;
        for (auto& p : dkgSessionHandlers) {
            LOCK2(p.second.cs, p.second.curSession->invCs);
            p.second.curSession->participatingNodes.emplace(pfrom->addr);
        }
        return;
    }

    if (vRecv.size() < 1) {
        LOCK(cs_main);
        Misbehaving(pfrom->id, 100);
        return;
    }

    // peek into the message and see which LLMQType it is. First byte of all messages is always the LLMQType
    Consensus::LLMQType llmqType = (Consensus::LLMQType)*vRecv.begin();
    if (!dkgSessionHandlers.count(llmqType)) {
        LOCK(cs_main);
        Misbehaving(pfrom->id, 100);
        return;
    }

    dkgSessionHandlers.at(llmqType).ProcessMessage(pfrom, strCommand, vRecv, connman);
}

bool CDKGSessionManager::AlreadyHave(const CInv& inv) const
{
    if (!sporkManager.IsSporkActive(SPORK_17_QUORUM_DKG_ENABLED))
        return false;

    for (auto& p : dkgSessionHandlers) {
        auto& dkgType = p.second;
        if (dkgType.pendingContributions.HasSeen(inv.hash)
            || dkgType.pendingComplaints.HasSeen(inv.hash)
            || dkgType.pendingJustifications.HasSeen(inv.hash)
            || dkgType.pendingPrematureCommitments.HasSeen(inv.hash)) {
            return true;
        }
    }
    return false;
}

bool CDKGSessionManager::GetContribution(const uint256& hash, CDKGContribution& ret) const
{
    if (!sporkManager.IsSporkActive(SPORK_17_QUORUM_DKG_ENABLED))
        return false;

    for (auto& p : dkgSessionHandlers) {
        auto& dkgType = p.second;
        LOCK2(dkgType.cs, dkgType.curSession->invCs);
        if (dkgType.phase < QuorumPhase_Initialized || dkgType.phase > QuorumPhase_Contribute) {
            continue;
        }
        auto it = dkgType.curSession->contributions.find(hash);
        if (it != dkgType.curSession->contributions.end()) {
            ret = it->second;
            return true;
        }
    }
    return false;
}

bool CDKGSessionManager::GetComplaint(const uint256& hash, CDKGComplaint& ret) const
{
    if (!sporkManager.IsSporkActive(SPORK_17_QUORUM_DKG_ENABLED))
        return false;

    for (auto& p : dkgSessionHandlers) {
        auto& dkgType = p.second;
        LOCK2(dkgType.cs, dkgType.curSession->invCs);
        if (dkgType.phase < QuorumPhase_Contribute || dkgType.phase > QuorumPhase_Complain) {
            continue;
        }
        auto it = dkgType.curSession->complaints.find(hash);
        if (it != dkgType.curSession->complaints.end()) {
            ret = it->second;
            return true;
        }
    }
    return false;
}

bool CDKGSessionManager::GetJustification(const uint256& hash, CDKGJustification& ret) const
{
    if (!sporkManager.IsSporkActive(SPORK_17_QUORUM_DKG_ENABLED))
        return false;

    for (auto& p : dkgSessionHandlers) {
        auto& dkgType = p.second;
        LOCK2(dkgType.cs, dkgType.curSession->invCs);
        if (dkgType.phase < QuorumPhase_Complain || dkgType.phase > QuorumPhase_Justify) {
            continue;
        }
        auto it = dkgType.curSession->justifications.find(hash);
        if (it != dkgType.curSession->justifications.end()) {
            ret = it->second;
            return true;
        }
    }
    return false;
}

bool CDKGSessionManager::GetPrematureCommitment(const uint256& hash, CDKGPrematureCommitment& ret) const
{
    if (!sporkManager.IsSporkActive(SPORK_17_QUORUM_DKG_ENABLED))
        return false;

    for (auto& p : dkgSessionHandlers) {
        auto& dkgType = p.second;
        LOCK2(dkgType.cs, dkgType.curSession->invCs);
        if (dkgType.phase < QuorumPhase_Justify || dkgType.phase > QuorumPhase_Commit) {
            continue;
        }
        auto it = dkgType.curSession->prematureCommitments.find(hash);
        if (it != dkgType.curSession->prematureCommitments.end() && dkgType.curSession->validCommitments.count(hash)) {
            ret = it->second;
            return true;
        }
    }
    return false;
}

void CDKGSessionManager::WriteVerifiedVvecContribution(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& proTxHash, const BLSVerificationVectorPtr& vvec)
{
    evoDb.GetRawDB().Write(std::make_tuple(DB_VVEC, (uint8_t)llmqType, quorumHash, proTxHash), *vvec);
}

void CDKGSessionManager::WriteVerifiedSkContribution(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& proTxHash, const CBLSSecretKey& skContribution)
{
    evoDb.GetRawDB().Write(std::make_tuple(DB_SKCONTRIB, (uint8_t)llmqType, quorumHash, proTxHash), skContribution);
}

bool CDKGSessionManager::GetVerifiedContributions(Consensus::LLMQType llmqType, const uint256& quorumHash, const std::vector<bool>& validMembers, std::vector<uint16_t>& memberIndexesRet, std::vector<BLSVerificationVectorPtr>& vvecsRet, BLSSecretKeyVector& skContributionsRet)
{
    auto members = CLLMQUtils::GetAllQuorumMembers(llmqType, quorumHash);

    if (validMembers.size() != members.size()) {
        // should never happen as we should always call this method with correct params
        return false;
    }

    memberIndexesRet.clear();
    vvecsRet.clear();
    skContributionsRet.clear();
    memberIndexesRet.reserve(members.size());
    vvecsRet.reserve(members.size());
    skContributionsRet.reserve(members.size());
    for (size_t i = 0; i < members.size(); i++) {
        if (validMembers[i]) {
            BLSVerificationVectorPtr vvec;
            CBLSSecretKey skContribution;
            if (!GetVerifiedContribution(llmqType, quorumHash, members[i]->proTxHash, vvec, skContribution)) {
                return false;
            }

            memberIndexesRet.emplace_back(i);
            vvecsRet.emplace_back(vvec);
            skContributionsRet.emplace_back(skContribution);
        }
    }
    return true;
}

bool CDKGSessionManager::GetVerifiedContribution(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& proTxHash, BLSVerificationVectorPtr& vvecRet, CBLSSecretKey& skContributionRet)
{
    LOCK(contributionsCacheCs);
    auto cacheKey = std::make_tuple(llmqType, quorumHash, proTxHash);
    auto it = contributionsCache.find(cacheKey);
    if (it != contributionsCache.end()) {
        vvecRet = it->second.first;
        skContributionRet = it->second.second;
        return true;
    }

    BLSVerificationVector vvec;
    BLSVerificationVectorPtr vvecPtr;
    CBLSSecretKey skContribution;
    if (evoDb.GetRawDB().Read(std::make_tuple(DB_VVEC, (uint8_t)llmqType, quorumHash, proTxHash), vvec)) {
        vvecPtr = std::make_shared<BLSVerificationVector>(std::move(vvec));
    }
    evoDb.GetRawDB().Read(std::make_tuple(DB_SKCONTRIB, (uint8_t)llmqType, quorumHash, proTxHash), skContribution);

    it = contributionsCache.emplace(cacheKey, std::make_pair(vvecPtr, skContribution)).first;

    vvecRet = it->second.first;
    skContributionRet = it->second.second;

    return true;
}

}
