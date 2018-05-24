// Copyright (c) 2018 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "quorums_signing.h"
#include "validation.h"
#include "activemasternode.h"

#include "net_processing.h"
#include "netmessagemaker.h"
#include "scheduler.h"
#include "cxxtimer.hpp"

#include "init.h"

#include <algorithm>
#include <limits>

namespace llmq
{

static uint256 MakeSignHash(Consensus::LLMQType llmqType, const uint256& quorumHash, const uint256& id, const uint256& msgHash)
{
    CHashWriter h(SER_GETHASH, 0);
    h << (uint8_t)llmqType;
    h << quorumHash;
    h << id;
    h << msgHash;
    return h.GetHash();
}

// works for sig shares and recovered sigs
template<typename T>
static uint256 MakeSignHash(const T& s)
{
    return MakeSignHash((Consensus::LLMQType)s.llmqType, s.quorumHash, s.id, s.msgHash);
}

template<typename M>
static std::pair<typename M::const_iterator, typename M::const_iterator> FindBySignHash(const M& m, const uint256& signHash)
{
    return std::make_pair(
            m.lower_bound(std::make_pair(signHash, (uint16_t)0)),
            m.upper_bound(std::make_pair(signHash, std::numeric_limits<uint16_t>::max()))
    );
}
template<typename M>
static size_t CountBySignHash(const M& m, const uint256& signHash)
{
    auto itPair = FindBySignHash(m, signHash);
    size_t count = 0;
    while (itPair.first != itPair.second) {
        count++;
        ++itPair.first;
    }
    return count;
}

template<typename M>
static void EraseBySignHash(M& m, const uint256& signHash)
{
    auto itPair = FindBySignHash(m, signHash);
    m.erase(itPair.first, itPair.second);
}

CSigningManager* quorumsSigningManager;

CSigningManager::CSigningManager(CEvoDB& _evoDb, CBLSWorker& _blsWorker) :
    evoDb(_evoDb),
    blsWorker(_blsWorker),
    workQueue(0)
{
    workThread = std::thread([this]() {
        RenameThread("quorum-signing");
        WorkThreadMain();
    });
}

CSigningManager::~CSigningManager()
{
    {
        std::unique_lock<std::mutex> l(workQueueMutex);
        stopWorkThread = true;
    }
    if (workThread.joinable()) {
        workThread.join();
    }
}

bool CSigningManager::AlreadyHave(const CInv& inv)
{
    LOCK(cs);
    return inv.type == MSG_QUORUM_RECOVERED_SIG && recoveredSigs.count(inv.hash);
}

bool CSigningManager::GetRecoveredSig(const uint256& hash, CRecoveredSig& ret)
{
    LOCK(cs);
    auto it = recoveredSigs.find(hash);
    if (it == recoveredSigs.end()) {
        return false;
    }
    ret = it->second;
    return true;
}

void CSigningManager::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    // non-masternodes are only interested in recovered signatures
    if (strCommand != NetMsgType::QSIGREC && (!fMasternodeMode || activeMasternodeInfo.proTxHash.IsNull())) {
        return;
    }

    if (strCommand == NetMsgType::QSIGSHARESINV) {
        CSigSharesInv inv;
        vRecv >> inv;
        ProcessMessageSigSharesInv(pfrom, inv, connman);
    } else if (strCommand == NetMsgType::QGETSIGSHARES) {
        CSigSharesInv inv;
        vRecv >> inv;
        ProcessMessageGetSigShares(pfrom, inv, connman);
    } else if (strCommand == NetMsgType::QBSIGSHARES) {
        CBatchedSigShares batchedSigShares;
        vRecv >> batchedSigShares;
        ProcessMessageBatchedSigShares(pfrom, batchedSigShares, connman);
    } else if (strCommand == NetMsgType::QSIGREC) {
        CRecoveredSig recoveredSig;
        vRecv >> recoveredSig;
        ProcessMessageRecoveredSig(pfrom, recoveredSig, connman);
    }
}

bool CSigningManager::VerifySigSharesInv(NodeId from, const CSigSharesInv& inv)
{
    Consensus::LLMQType llmqType = (Consensus::LLMQType)inv.llmqType;
    if (!Params().GetConsensus().llmqs.count(llmqType) || inv.signHash.IsNull()) {
        LOCK(cs_main);
        Misbehaving(from, 100);
        return false;
    }

    if (!fMasternodeMode || activeMasternodeInfo.proTxHash.IsNull()) {
        return false;
    }

    size_t quorumSize = (size_t)Params().GetConsensus().llmqs.at(llmqType).size;

    if (inv.inv.size() != quorumSize) {
        LOCK(cs_main);
        Misbehaving(from, 100);
        return false;
    }
    return true;
}

void CSigningManager::ProcessMessageSigSharesInv(CNode* pfrom, const CSigSharesInv& inv, CConnman& connman)
{
    if (!VerifySigSharesInv(pfrom->id, inv)) {
        return;
    }

    LOCK(cs);
    if (recoveredSessions.count(inv.signHash)) {
        return;
    }

    LogPrint("llmq", "CSigningManager::%s -- inv={%s}, node=%d\n", __func__, inv.ToString(), pfrom->id);

    auto& nodeState = nodeStates[pfrom->id];
    nodeState.MarkAnnounced(inv.signHash, inv);
    nodeState.MarkKnows(inv.signHash, inv);
}

void CSigningManager::ProcessMessageGetSigShares(CNode* pfrom, const CSigSharesInv& inv, CConnman& connman)
{
    if (!VerifySigSharesInv(pfrom->id, inv)) {
        return;
    }

    LOCK(cs);
    if (recoveredSessions.count(inv.signHash)) {
        return;
    }

    LogPrint("llmq", "CSigningManager::%s -- inv={%s}, node=%d\n", __func__, inv.ToString(), pfrom->id);

    auto& nodeState = nodeStates[pfrom->id];
    nodeState.MarkRequested(inv.signHash, inv);
    nodeState.MarkKnows(inv.signHash, inv);
}

void CSigningManager::ProcessMessageBatchedSigShares(CNode* pfrom, const llmq::CBatchedSigShares& batchedSigShares, CConnman& connman)
{
    bool ban = false;
    if (!PreVerifyBatchedSigShares(pfrom->id, batchedSigShares, ban)) {
        if (ban) {
            LOCK(cs_main);
            Misbehaving(pfrom->id, 100);
            return;
        }
        return;
    }

    std::vector<CSigShare> sigShares;
    sigShares.reserve(batchedSigShares.sigShares.size());

    LOCK(cs);
    auto& nodeState = nodeStates[pfrom->id];

    for (size_t i = 0; i < batchedSigShares.sigShares.size(); i++) {
        CSigShare sigShare = batchedSigShares.RebuildSigShare(i);
        nodeState.requestedSigShares.erase(sigShare.GetKey());

        // TODO track invalid sig shares received?
        // It's important to only skip seen *valid* sig shares here. If a node sends us a
        // batch of mostly valid sig shares with a single invalid one and thus batched
        // verification fails, we'd skip the valid ones in the future if received from other nodes
        if (this->sigShares.count(sigShare.GetKey())) {
            continue;
        }
        if (recoveredSigsForIds.count(std::make_pair((Consensus::LLMQType)sigShare.llmqType, sigShare.id))) {
            // skip sig share if we already have a recovered sig
            continue;
        }
        sigShares.emplace_back(sigShare);
    }

    LogPrint("llmq", "CSigningManager::%s -- shares=%d, new=%d, inv={%s}, node=%d\n", __func__,
             batchedSigShares.sigShares.size(), sigShares.size(), batchedSigShares.ToInv().ToString(), pfrom->id);

    if (sigShares.empty()) {
        return;
    }

    for (auto& s : sigShares) {
        nodeState.pendingIncomingSigShares.emplace(s.GetKey(), s);
    }
}

void CSigningManager::ProcessMessageRecoveredSig(CNode* pfrom, const llmq::CRecoveredSig& recoveredSig, CConnman& connman)
{
    bool ban = false;
    if (!PreVerifyRecoveredSig(pfrom->id, recoveredSig, ban)) {
        if (ban) {
            LOCK(cs_main);
            Misbehaving(pfrom->id, 100);
            return;
        }
        return;
    }

    LOCK(cs);
    // It's important to only skip seen *valid* sig shares here. See comment for CBatchedSigShare
    // We don't receive recovered sigs in batches, but we do batched verification per node on these
    if (recoveredSigs.count(recoveredSig.GetHash())) {
        return;
    }

    LogPrint("llmq", "CSigningManager::%s -- signHash=%s, node=%d\n", __func__, MakeSignHash(recoveredSig).ToString(), pfrom->id);

    auto& nodeState = nodeStates[pfrom->id];
    nodeState.pendingIncomingRecSigs.emplace(recoveredSig.GetHash(), recoveredSig);
}

bool CSigningManager::PreVerifyBatchedSigShares(NodeId nodeId, const llmq::CBatchedSigShares& batchedSigShares, bool& retBan)
{
    retBan = false;

    auto llmqType = (Consensus::LLMQType)batchedSigShares.llmqType;
    if (!Params().GetConsensus().llmqs.count(llmqType)) {
        retBan = true;
        return false;
    }

    CQuorumCPtr quorum;
    {
        LOCK(cs_main);

        quorum = quorumManager->GetQuorum(llmqType, batchedSigShares.quorumHash);
        if (!quorum) {
            LogPrintf("CSigningManager::%s -- quorum %s not found, node=%d\n", __func__,
                      batchedSigShares.quorumHash.ToString(), nodeId);
            return false;
        }
        if (!IsQuorumActive(llmqType, quorum->quorumHash)) {
            return false;
        }
        if (!quorum->IsMember(activeMasternodeInfo.proTxHash)) {
            return false;
        }
        if (quorum->quorumVvec == nullptr) {
            // TODO we should allow to ask other nodes for the quorum vvec
            LogPrintf("CSigningManager::%s -- we don't have the quorum vvec for %s, no verification possible. node=%d\n", __func__,
                      batchedSigShares.quorumHash.ToString(), nodeId);
            return false;
        }
    }

    std::set<uint16_t> dupMembers;

    for (size_t i = 0; i < batchedSigShares.sigShares.size(); i++) {
        auto quorumMember = batchedSigShares.sigShares[i].first;
        auto& sigShare = batchedSigShares.sigShares[i].second;
        if (!sigShare.IsValid()) {
            // TODO banning might be bad here actually
            retBan = true;
            return false;
        }
        if (!dupMembers.emplace(quorumMember).second) {
            // TODO banning might be bad here actually
            retBan = true;
            return false;
        }

        if (quorumMember >= quorum->members.size()) {
            LogPrintf("CSigningManager::%s -- quorumMember out of bounds\n", __func__);
            // TODO banning might be bad here actually
            retBan = true;
            return false;
        }
        if (!quorum->validMembers[quorumMember]) {
            LogPrintf("CSigningManager::%s -- quorumMember not valid\n", __func__);
            // TODO banning might be bad here actually
            retBan = true;
            return false;
        }
    }
    return true;
}

bool CSigningManager::PreVerifyRecoveredSig(NodeId nodeId, const llmq::CRecoveredSig& recoveredSig, bool& retBan)
{
    retBan = false;

    auto llmqType = (Consensus::LLMQType)recoveredSig.llmqType;
    if (!Params().GetConsensus().llmqs.count(llmqType)) {
        retBan = true;
        return false;
    }

    CQuorumCPtr quorum;
    {
        LOCK(cs_main);

        quorum = quorumManager->GetQuorum(llmqType, recoveredSig.quorumHash);
        if (!quorum) {
            LogPrintf("CSigningManager::%s -- quorum %s not found, node=%d\n", __func__,
                      recoveredSig.quorumHash.ToString(), nodeId);
            return false;
        }
        if (!IsQuorumActive(llmqType, quorum->quorumHash)) {
            return false;
        }
    }

    if (!recoveredSig.sig.IsValid()) {
        // TODO banning might be bad here actually
        retBan = true;
        return false;
    }

    return true;
}

void CSigningManager::ProcessPendingIncomingSigs(CConnman& connman)
{
    // process recovered sigs first, as they might result in pending sig shares becoming
    // obsolete and thus we can speed up processing
    ProcessPendingRecoveredSigs(connman);
    ProcessPendingSigShares(connman);
}

template<typename Continue, typename Callback>
static void IterateNodeStatesRandom(std::map<NodeId, CSigSharesNodeState>& nodeStates, Continue&& cont, Callback&& callback, FastRandomContext& rnd)
{
    std::vector<std::map<NodeId, CSigSharesNodeState>::iterator> rndNodeStates;
    rndNodeStates.reserve(nodeStates.size());
    for (auto it = nodeStates.begin(); it != nodeStates.end(); ++it) {
        rndNodeStates.emplace_back(it);
    }
    if (rndNodeStates.empty()) {
        return;
    }
    std::random_shuffle(rndNodeStates.begin(), rndNodeStates.end(), rnd);

    size_t idx = 0;
    while (!rndNodeStates.empty() && cont()) {
        auto nodeId = rndNodeStates[idx]->first;
        auto& ns = rndNodeStates[idx]->second;

        if (callback(nodeId, ns)) {
            idx = (idx + 1) % rndNodeStates.size();
        } else {
            rndNodeStates.erase(rndNodeStates.begin() + idx);
            if (rndNodeStates.empty()) {
                break;
            }
            idx %= rndNodeStates.size();
        }
    }
}

void CSigningManager::ProcessPendingSigShares(CConnman& connman)
{
    std::map<NodeId, std::vector<CSigShare>> groupedByNode;
    {
        LOCK(cs);
        if (nodeStates.empty()) {
            return;
        }

        std::set<std::pair<NodeId, uint256>> uniqueSignHashes;
        IterateNodeStatesRandom(nodeStates, [&]() {
            return uniqueSignHashes.size() < 32;
        }, [&](NodeId nodeId, CSigSharesNodeState& ns) {
            if (ns.pendingIncomingSigShares.empty()) {
                return false;
            }
            auto& sigShare = ns.pendingIncomingSigShares.begin()->second;

            bool alreadyHave = this->sigShares.count(sigShare.GetKey()) ||
                               recoveredSigsForIds.count(std::make_pair((Consensus::LLMQType)sigShare.llmqType, sigShare.id));
            if (!alreadyHave) {
                uniqueSignHashes.emplace(nodeId, sigShare.GetSignHash());
                groupedByNode[nodeId].emplace_back(sigShare);
            }
            ns.pendingIncomingSigShares.erase(ns.pendingIncomingSigShares.begin());
            return !ns.pendingIncomingSigShares.empty();
        }, rnd);

        if (groupedByNode.empty()) {
            return;
        }
    }

    std::map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr> quorums;
    {
        LOCK(cs_main);
        for (auto& p : groupedByNode) {
            for (auto& sigShare : p.second) {
                auto llmqType = (Consensus::LLMQType) sigShare.llmqType;

                auto k = std::make_pair(llmqType, sigShare.quorumHash);
                if (quorums.count(k)) {
                    continue;
                }

                CQuorumCPtr quorum = quorumManager->GetQuorum(llmqType, sigShare.quorumHash);
                assert(quorum != nullptr);
                quorums.emplace(k, quorum);
            }
        }
    }

    // TODO describe this

    const size_t batchedVerifyCount = 8;
    std::set<NodeId> invalidNodes;

    while (!groupedByNode.empty()) {
        std::map<NodeId, std::vector<CSigShare>> batched;
        size_t sigShareCount = 0;
        for (size_t i = 0; i < batchedVerifyCount && !groupedByNode.empty(); i++) {
            auto it = groupedByNode.begin();
            NodeId nodeId = it->first;
            auto& sigShares = it->second;

            sigShareCount += sigShares.size();
            batched.emplace(nodeId, std::move(sigShares));
            groupedByNode.erase(it);
        }

        std::vector<CSigShare> verifySigShares;
        std::set<SigShareKey> dupSet;
        verifySigShares.reserve(sigShareCount);
        {
            LOCK(cs);
            for (auto& p : batched) {
                for (auto& sigShare : p.second) {
                    if (!dupSet.emplace(sigShare.GetKey()).second) {
                        // don't include duplicates from different nodes
                        // if there are duplicates where one of both is invalid, batch verification will fail and we'll
                        // revert to per-node verification to figure out which one is the valid one
                        continue;
                    }
                    if (this->sigShares.count(sigShare.GetKey())) {
                        // previous batch validated this one
                        continue;
                    }
                    if (recoveredSigsForIds.count(std::make_pair((Consensus::LLMQType)sigShare.llmqType, sigShare.id))) {
                        // previous batch resulted in recovery
                        continue;
                    }
                    verifySigShares.emplace_back(sigShare);
                }
            }
        }

        std::set<NodeId> validNodes;
        // if verifySigShares is empty, a previous batch verified all sig shares from the current batch,
        // so the current batch is also valid
        bool batchValid = verifySigShares.empty() || VerifyPendingSigShares(verifySigShares, quorums);
        if (!batchValid) {
            // at least one node sent at least one invalid sig share, let's figure out who it was
            if (batched.size() == 1) {
                auto nodeId = batched.begin()->first;
                invalidNodes.emplace(nodeId);
            } else {
                for (auto& p : batched) {
                    auto nodeId = p.first;
                    auto& sigShares = p.second;
                    bool valid = VerifyPendingSigShares(sigShares, quorums);
                    if (!valid) {
                        invalidNodes.emplace(nodeId);
                    } else {
                        validNodes.emplace(nodeId);
                    }
                }
            }
        } else {
            // all valid
            for (auto& p : batched) {
                validNodes.emplace(p.first);
            }
        }

        if (!invalidNodes.empty()) {
            LOCK(cs_main);
            for (auto nodeId : invalidNodes) {
                LogPrintf("CSigningManager::%s -- invalid sig shares. banning node=%d\n", __func__,
                          nodeId);
                Misbehaving(nodeId, 100);
            }
        }

        for (auto nodeId : validNodes) {
            auto& sigShares = batched.at(nodeId);
            ProcessPendingSigSharesFromNode(nodeId, sigShares, quorums, connman);
        }
    }
}

void CSigningManager::ProcessPendingRecoveredSigs(CConnman& connman)
{
    std::map<NodeId, std::list<CRecoveredSig>> groupedByNode;
    {
        LOCK(cs);
        if (nodeStates.empty()) {
            return;
        }

        std::set<std::pair<NodeId, uint256>> uniqueSignHashes;
        IterateNodeStatesRandom(nodeStates, [&]() {
            return uniqueSignHashes.size() < 32;
        }, [&](NodeId nodeId, CSigSharesNodeState& ns) {
            if (ns.pendingIncomingRecSigs.empty()) {
                return false;
            }
            auto& recSig = ns.pendingIncomingRecSigs.begin()->second;

            bool alreadyHave = this->recoveredSigs.count(recSig.GetHash()) != 0;
            if (!alreadyHave) {
                uniqueSignHashes.emplace(nodeId, MakeSignHash(recSig));
                groupedByNode[nodeId].emplace_back(recSig);
            }
            ns.pendingIncomingRecSigs.erase(ns.pendingIncomingRecSigs.begin());
            return !ns.pendingIncomingRecSigs.empty();
        }, rnd);

        if (groupedByNode.empty()) {
            return;
        }
    }

    std::map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr> quorums;
    {
        LOCK(cs_main);
        for (auto& p : groupedByNode) {
            NodeId nodeId = p.first;
            auto& v = p.second;

            for (auto it = v.begin(); it != v.end();) {
                auto& recSig = *it;

                Consensus::LLMQType llmqType = (Consensus::LLMQType) recSig.llmqType;
                auto quorumKey = std::make_pair((Consensus::LLMQType)recSig.llmqType, recSig.quorumHash);
                if (!quorums.count(quorumKey)) {
                    //TODO only recent quorums
                    CQuorumCPtr quorum = quorumManager->GetQuorum(llmqType, recSig.quorumHash);
                    if (!quorum) {
                        LogPrintf("CSigningManager::%s -- quorum %s not found, node=%d\n", __func__,
                                  recSig.quorumHash.ToString(), nodeId);
                        it = v.erase(it);
                        continue;
                    }
                    if (!IsQuorumActive(llmqType, quorum->quorumHash)) {
                        LogPrintf("CSigningManager::%s -- quorum %s not active anymore, node=%d\n", __func__,
                                  recSig.quorumHash.ToString(), nodeId);
                        it = v.erase(it);
                        continue;
                    }

                    quorums.emplace(quorumKey, quorum);
                }

                ++it;
            }
        }
    }

    for (auto& p : groupedByNode) {
        NodeId nodeId = p.first;
        auto& v = p.second;

        if (!v.empty()) {
            // We do batched verification and processing per node, but this means that we might have duplicates
            // between nodes. We could not remove these duplicates before as it might result in nodes tricking us into
            // skipping non-processes recovered sigs. However, after the calls to ProcessRecoveredSig in this loop
            // we might know more valid recovered sigs which we can safely skip
            LOCK(cs);
            v.remove_if([this](const CRecoveredSig& rs) {
                return this->recoveredSigs.count(rs.GetHash());
            });
        }
        if (v.empty()) {
            continue;
        }

        ProcessPendingRecoveredSigsFromNode(nodeId, v, quorums, connman);
    }
}

bool CSigningManager::VerifyPendingSigShares(const std::vector<CSigShare>& sigShares, const std::map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr>& quorums)
{
    std::map<uint256, std::set<size_t>> groupedBySignHash;

    for (size_t i = 0; i < sigShares.size(); i++) {
        auto& sigShare = sigShares[i];
        groupedBySignHash[sigShare.GetSignHash()].emplace(i);
    }

    // insecure verification is fine here as the public key shares are trusted due to the DKG protocol
    // (no rogue public key attack possible)

    CBLSSignature aggSig;
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<uint256> hashes;

    cxxtimer::Timer aggTimer(false);
    cxxtimer::Timer verifyTimer(false);
    cxxtimer::Timer totalTimer(true);

    aggTimer.start();
    for (auto& p2 : groupedBySignHash) {
        auto& signHash = p2.first;
        auto& v = p2.second;

        CBLSSignature aggSig2;
        CBLSPublicKey aggPubKey2;

        for (auto idx : v) {
            auto& sigShare = sigShares[idx];
            auto quorumKey = std::make_pair((Consensus::LLMQType)sigShare.llmqType, sigShare.quorumHash);
            auto pubKeyShare = quorums.at(quorumKey)->GetPubKeyShare(sigShare.quorumMember);
            if (!pubKeyShare.IsValid()) {
                // this should really not happen (we already ensured we have the quorum vvec,
                // so we should also be able to create all pubkey shares)
                return false;
            }
            if (!aggSig2.IsValid()) {
                aggSig2 = sigShare.sigShare;
                aggPubKey2 = pubKeyShare;
            } else {
                aggSig2.AggregateInsecure(sigShare.sigShare);
                aggPubKey2.AggregateInsecure(pubKeyShare);
            }
            if (!aggSig2.IsValid() || !aggPubKey2.IsValid()) {
                return false;
            }
        }

        if (pubKeys.empty()) {
            aggSig = aggSig2;
        } else {
            aggSig.AggregateInsecure(aggSig2);
        }
        if (!aggSig.IsValid()) {
            return false;
        }
        pubKeys.emplace_back(aggPubKey2);
        hashes.emplace_back(signHash);
    }
    aggTimer.stop();

    std::string invStr = "";
    if (LogAcceptCategory("llmq")) {
        std::map<uint256, CSigSharesInv> invs;
        for (auto& s : sigShares) {
            auto& inv = invs[s.GetSignHash()];
            if (inv.inv.empty()) {
                inv.Init((Consensus::LLMQType) s.llmqType, s.GetSignHash());
            }
            inv.inv[s.quorumMember] = true;
        }
        for (auto& p : invs) {
            if (!invStr.empty()) {
                invStr += ", ";
            }
            invStr += strprintf("{%s}", p.second.ToString());
        }
    }

    verifyTimer.start();
    bool valid = aggSig.VerifyInsecureAggregated(pubKeys, hashes);
    verifyTimer.stop();
    if (!valid) {
        LogPrint("llmq", "CSigningManager::%s -- invalid sig shares. count=%d, invs={%s}, at=%d, vt=%d, tt=%d\n", __func__,
                  sigShares.size(), invStr, aggTimer.count(), verifyTimer.count(), totalTimer.count());
    } else {
        LogPrint("llmq", "CSigningManager::%s -- valid sig shares. count=%d, invs={%s}, at=%d, vt=%d, tt=%d\n", __func__,
                  sigShares.size(), invStr, aggTimer.count(), verifyTimer.count(), totalTimer.count());
    }

    return valid;
}

// It's ensured that no duplicates are passed to this method
void CSigningManager::ProcessPendingSigSharesFromNode(NodeId nodeId, const std::vector<CSigShare>& sigShares, const std::map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr>& quorums, CConnman& connman)
{
    auto& nodeState = nodeStates[nodeId];

    cxxtimer::Timer t(true);
    for (auto& sigShare : sigShares) {
        // he sent us some valid sig shares, so he must be part of this quorum and is thus interested in our sig shares as well
        // if this is the first time we received a sig share from this node, we won't announce the currently locally known sig shares to him.
        // only the upcoming sig shares will be announced to him. this means the first signing session for a fresh quorum will be a bit
        // slower than for older ones. TODO: fix this (risk of DoS when announcing all at once?)
        auto quorumKey = std::make_pair((Consensus::LLMQType)sigShare.llmqType, sigShare.quorumHash);
        nodeState.interestedIn.emplace(quorumKey);

        ProcessSigShare(nodeId, sigShare, connman, quorums.at(quorumKey));
    }
    t.stop();

    LogPrint("llmq", "CSigningManager::%s -- processed sigShare batch. shares=%d, time=%d, node=%d\n", __func__,
              sigShares.size(), t.count(), nodeId);
}

// It's ensured that no duplicates are passed to this method
void CSigningManager::ProcessPendingRecoveredSigsFromNode(NodeId nodeId, const std::list<CRecoveredSig>& recoveredSigs,
                                                          const std::map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr>& quorums,
                                                          CConnman& connman)
{
    const size_t batchedVerifyCount = 8;

    CBLSSignature aggSig;
    std::vector<CBLSPublicKey> pubKeys;
    std::vector<uint256> signHashes;
    pubKeys.reserve(recoveredSigs.size());
    signHashes.reserve(recoveredSigs.size());

    cxxtimer::Timer verifyTimer(false);
    size_t verifyCount = 0;
    auto verifyBatch = [&]() {
        verifyTimer.start();
        if (!aggSig.VerifyInsecureAggregated(pubKeys, signHashes)) {
            LogPrintf("CSigningManager::ProcessPendingRecoveredSigsFromNode -- invalid recovered sigs. time=%d, node=%d\n", verifyTimer.count(), nodeId);
            LOCK(cs_main);
            Misbehaving(nodeId, 100);
            return false;
        }
        verifyTimer.stop();
        verifyCount += pubKeys.size();
        aggSig = CBLSSignature();
        pubKeys.clear();
        signHashes.clear();
        return true;
    };

    for (auto& rs : recoveredSigs) {
        auto signHash = MakeSignHash(rs);

        if (pubKeys.empty()) {
            aggSig = rs.sig;
        } else {
            aggSig.AggregateInsecure(rs.sig);
        }
        if (!aggSig.IsValid()) {
            {
                LOCK(cs_main);
                Misbehaving(nodeId, 100);
            }
            return;
        }

        signHashes.emplace_back(signHash);

        auto& quorum = quorums.at(std::make_pair((Consensus::LLMQType)rs.llmqType, rs.quorumHash));
        pubKeys.emplace_back(quorum->quorumPublicKey);

        // we verify in batches, as otherwise an attacker might send thousands of messages at once and force us to
        // perform thousands of BLS pairings, effectively DoS'ing us
        // As the attacker can't craft valid signatures, he'll only be able to send us a few valid ones before we
        // encounter the first invalid one, so the attack is detected early and the attacker banned
        if (pubKeys.size() >= batchedVerifyCount) {
            if (!verifyBatch()) {
                return;
            }
        }
    }
    // Verify remaining signatures
    if (!pubKeys.empty() && !verifyBatch()) {
        return;
    }
    LogPrintf("CSigningManager::%s -- verified recovered sig(s). count=%d, vt=%d, node=%d\n", __func__, verifyCount, verifyTimer.count(), nodeId);

    for (auto& rs : recoveredSigs) {
        auto& quorum = quorums.at(std::make_pair((Consensus::LLMQType)rs.llmqType, rs.quorumHash));
        ProcessRecoveredSig(nodeId, rs, quorum, connman);
    }
}

// sig shares are already verified when entering this method
void CSigningManager::ProcessSigShare(NodeId nodeId, const CSigShare& sigShare, CConnman& connman, const CQuorumCPtr& quorum)
{
    auto llmqType = quorum->params.type;

    bool canTryRecovery = false;

    // prepare node set for direct-push in case this is our sig share
    std::set<NodeId> quorumNodes;
    if (sigShare.quorumMember == quorum->GetMemberIndex(activeMasternodeInfo.proTxHash)) {
        quorumNodes = connman.GetMasternodeQuorumNodes((Consensus::LLMQType) sigShare.llmqType, sigShare.quorumHash);
        // make sure node states are created for these nodes (we might have not received any message from these yet)
        for (auto otherNodeId : quorumNodes) {
            nodeStates[otherNodeId].interestedIn.emplace(std::make_pair((Consensus::LLMQType)sigShare.llmqType, sigShare.quorumHash));
        }
    }

    {
        LOCK(cs);
        if (recoveredSigsForIds.count(std::make_pair(llmqType, sigShare.id))) {
            return;
        }

        sigShares.emplace(sigShare.GetKey(), sigShare);
        sigSharesToAnnounce.emplace(sigShare.GetKey());
        firstSeenForSessions.emplace(sigShare.GetSignHash(), GetTimeMillis());

        if (!quorumNodes.empty()) {
            // don't announce and wait for other nodes to request this share and directly send it to them
            // there is no way the other nodes know about this share as this is the one created on this node
            // this will also indicate interest to the other nodes in sig shares for this quorum
            for (auto& p : nodeStates) {
                if (!quorumNodes.count(p.first) && !p.second.interestedIn.count(std::make_pair((Consensus::LLMQType)sigShare.llmqType, sigShare.quorumHash))) {
                    continue;
                }
                p.second.MarkRequested((Consensus::LLMQType)sigShare.llmqType, sigShare.GetSignHash(), sigShare.quorumMember);
                p.second.MarkKnows((Consensus::LLMQType)sigShare.llmqType, sigShare.GetSignHash(), sigShare.quorumMember);
            }
        }

        size_t sigShareCount = CountBySignHash(sigShares, sigShare.GetSignHash());
        if (sigShareCount >= quorum->params.threshold) {
            canTryRecovery = true;
        }
    }

    if (canTryRecovery) {
        TryRecoverSig(quorum, sigShare.id, sigShare.msgHash, connman);
    }
}

void CSigningManager::TryRecoverSig(const CQuorumCPtr& quorum, const uint256& id, const uint256& msgHash, CConnman& connman)
{
    std::vector<CBLSSignature> sigSharesForRecovery;
    std::vector<CBLSId> idsForRecovery;
    {
        LOCK(cs);

        auto k = std::make_pair(quorum->params.type, id);

        if (recoveredSigsForIds.count(k)) {
            return;
        }

        auto signHash = MakeSignHash(quorum->params.type, quorum->quorumHash, id, msgHash);
        auto itPair = FindBySignHash(sigShares, signHash);

        sigSharesForRecovery.reserve((size_t) quorum->params.threshold);
        idsForRecovery.reserve((size_t) quorum->params.threshold);
        for (auto it = itPair.first; it != itPair.second && sigSharesForRecovery.size() < quorum->params.threshold; ++it) {
            auto& sigShare = it->second;
            sigSharesForRecovery.emplace_back(sigShare.sigShare);
            idsForRecovery.emplace_back(CBLSId::FromHash(quorum->members[sigShare.quorumMember]->proTxHash));
        }

        // check if we can recover the final signature
        if (sigSharesForRecovery.size() < quorum->params.threshold) {
            return;
        }
    }

    // now recover it
    cxxtimer::Timer t(true);
    CBLSSignature recoveredSig;
    if (!recoveredSig.Recover(sigSharesForRecovery, idsForRecovery)) {
        LogPrintf("CSigningManager::%s -- failed to recover signature. id=%s, msgHash=%s, time=%d\n", __func__,
                  id.ToString(), msgHash.ToString(), t.count());
    } else {
        LogPrintf("CSigningManager::%s -- recovered signature. id=%s, msgHash=%s, time=%d\n", __func__,
                  id.ToString(), msgHash.ToString(), t.count());

        CRecoveredSig rs;
        rs.llmqType = quorum->params.type;
        rs.quorumHash = quorum->quorumHash;
        rs.id = id;
        rs.msgHash = msgHash;
        rs.sig = recoveredSig;
        rs.UpdateHash();

        auto signHash = MakeSignHash(rs);
        bool valid = rs.sig.VerifyInsecure(quorum->quorumPublicKey, signHash);
        if (!valid) {
            // this should really not happen as we have verified all signature shares before
            LogPrintf("CSigningManager::%s -- own recovered signature is invalid. id=%s, msgHash=%s\n", __func__,
                      id.ToString(), msgHash.ToString());
            return;
        }

        ProcessRecoveredSig(-1, rs, quorum, connman);
    }
}

// signature must be verified already
void CSigningManager::ProcessRecoveredSig(NodeId nodeId, const CRecoveredSig& recoveredSig, const CQuorumCPtr& quorum, CConnman& connman)
{
    auto llmqType = (Consensus::LLMQType)recoveredSig.llmqType;

    {
        LOCK(cs_main);
        connman.RemoveAskFor(recoveredSig.GetHash());
    }

    {
        LOCK(cs);

        auto signHash = MakeSignHash(recoveredSig);

        LogPrintf("CSigningManager::%s -- valid recSig. signHash=%s, id=%s, msgHash=%s, node=%d\n", __func__,
                signHash.ToString(), recoveredSig.id.ToString(), recoveredSig.msgHash.ToString(), nodeId);

        if (recoveredSigsForIds.count(std::make_pair(llmqType, recoveredSig.id))) {
            // this should really not happen, as each masternode is participating in only one vote,
            // even if it's a member of multiple quorums. so a majority is only possible on one quorum and one msgHash per id
            LogPrintf("CSigningManager::%s -- conflicting recoveredSig for id=%s, msgHash=%s\n", __func__,
                      recoveredSig.id.ToString(), recoveredSig.msgHash.ToString());
            return;
        }

        auto key = std::make_pair(llmqType, recoveredSig.id);
        recoveredSigs.emplace(recoveredSig.GetHash(), recoveredSig);
        recoveredSigsForIds.emplace(key, recoveredSig.GetHash());
        recoveredSessions.emplace(signHash);

        RemoveSigSharesForSession(signHash);

        votedOnIds.erase(key);
    }

    CInv inv(MSG_QUORUM_RECOVERED_SIG, recoveredSig.GetHash());
    g_connman->RelayInv(inv);
}

bool CSigningManager::IsQuorumActive(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    AssertLockHeld(cs_main);

    auto& params = Params().GetConsensus().llmqs.at(llmqType);

    // sig shares and recovered sigs are only accepted from recent/active quorums
    // we allow one more active quorum as specified in consensus, as otherwise there is a small window where things could
    // fail while we are on the brink of a new quorum
    auto quorums = quorumManager->ScanQuorums(llmqType, (int)params.signingActiveQuorumCount + 1);
    for (auto& q : quorums) {
        if (q->quorumHash == quorumHash) {
            return true;
        }
    }
    return false;
}

void CSigningManager::AsyncSignIfMember(Consensus::LLMQType llmqType, const uint256& id, const uint256& msgHash)
{
    // delay around 30% of signing requests
    // in most cases, 70% of members signing should be more than enough to create a recovered signature
    // in some cases however, it might happen that we still fail, and then the delayed signatures come to the rescue
    // TODO does this really improve global performance?
    int64_t delay = 0;
    double r = GetRand(1000000) / 1000000.0;
    if (r >= 0.7) {
        delay = 4 * 1000;
    }

    PushWork(delay, [this, llmqType, id, msgHash]() {
        SignIfMember(llmqType, id, msgHash);
    });
}

void CSigningManager::SignIfMember(Consensus::LLMQType llmqType, const uint256& id, const uint256& msgHash)
{
    auto& params = Params().GetConsensus().llmqs.at(llmqType);

    if (!fMasternodeMode || activeMasternodeInfo.proTxHash.IsNull()) {
        return;
    }

    {
        LOCK(cs);

        auto key = std::make_pair(llmqType, id);
        auto it = votedOnIds.find(key);
        if (it != votedOnIds.end()) {
            if (it->second != msgHash) {
                LogPrintf("CSigningManager::%s -- already voted for id=%s and msgHash=%s. Not voting on conflicting msgHash=%s\n", __func__,
                          id.ToString(), it->second.ToString(), msgHash.ToString());
                return;
            }
        } else {
            votedOnIds.emplace(key, msgHash);
        }

        if (recoveredSigsForIds.count(key)) {
            // no need to sign it if we already have a recovered sig
            return;
        }
    }


    // This might end up giving different results on different members
    // This might happen when we are on the brink of confirming a new quorum
    // This gives a slight risk of not getting enough shares to recover a signature
    // But at least it shouldn't be possible to get conflicting recovered signatures
    // TODO fix this by re-signing when the next block arrives, but only when that block results in a change of the quorum list and no recovered signature has been created in the mean time
    CQuorumCPtr quorum = quorumManager->SelectQuorum(llmqType, id, params.signingActiveQuorumCount);
    if (!quorum) {
        LogPrintf("CSigningManager::%s -- failed to select quorum. id=%s, msgHash=%s\n", __func__, id.ToString(), msgHash.ToString());
        return;
    }

    if (!quorum->IsValidMember(activeMasternodeInfo.proTxHash)) {
        //LogPrintf("CSigningManager::%s -- we're not a valid member of quorum %s\n", __func__, quorum->quorumHash.ToString());
        return;
    }

    Sign(quorum, id, msgHash);
}

void CSigningManager::Sign(const CQuorumCPtr& quorum, const uint256& id, const uint256& msgHash) {
    cxxtimer::Timer t(true);

    {
        LOCK(cs);
        if (recoveredSigsForIds.count(std::make_pair(quorum->params.type, id))) {
            // no need to sign it if we already have a recovered sig
            return;
        }
    }

    if (!quorum->IsValidMember(activeMasternodeInfo.proTxHash)) {
        return;
    }

    CBLSSecretKey skShare = quorum->GetSkShare();
    if (!skShare.IsValid()) {
        LogPrintf("CSigningManager::%s -- we don't have our skShare for quorum %s\n", __func__, quorum->quorumHash.ToString());
        return;
    }

    int memberIdx = quorum->GetMemberIndex(activeMasternodeInfo.proTxHash);
    if (memberIdx == -1) {
        // this should really not happen (IsValidMember gave true)
        return;
    }

    CSigShare sigShare;
    sigShare.llmqType = quorum->params.type;
    sigShare.quorumHash = quorum->quorumHash;
    sigShare.id = id;
    sigShare.msgHash = msgHash;
    sigShare.quorumMember = (uint16_t)memberIdx;
    uint256 signHash = MakeSignHash(sigShare);

    sigShare.sigShare = skShare.Sign(signHash);
    if (!sigShare.sigShare.IsValid()) {
        LogPrintf("CSigningManager::%s -- failed to sign sigShare. id=%s, msgHash=%s, time=%s\n", __func__,
                  sigShare.id.ToString(), sigShare.msgHash.ToString(), t.count());
        return;
    }

    sigShare.UpdateKey();

    LogPrintf("CSigningManager::%s -- signed sigShare. id=%s, msgHash=%s, time=%s\n", __func__,
              sigShare.id.ToString(), sigShare.msgHash.ToString(), t.count());
    ProcessSigShare(-1, sigShare, *g_connman, quorum);
}

// cs must be held
void CSigningManager::CollectSigSharesToRequest(std::map<NodeId, std::map<uint256, CSigSharesInv>>& sigSharesToRequest)
{
    int64_t now = GetTimeMillis();
    std::map<SigShareKey, std::vector<NodeId>> nodesBySigShares;

    const size_t maxRequestsForNode = 32;

    // avoid requesting from same nodes all the time
    std::vector<NodeId> shuffledNodeIds;
    shuffledNodeIds.reserve(nodeStates.size());
    for (auto& p : nodeStates) {
        if (p.second.sessions.empty()) {
            continue;
        }
        shuffledNodeIds.emplace_back(p.first);
    }
    std::random_shuffle(shuffledNodeIds.begin(), shuffledNodeIds.end(), rnd);

    for (auto& nodeId : shuffledNodeIds) {
        auto& nodeState = nodeStates[nodeId];

        for (auto it = nodeState.requestedSigShares.begin(); it != nodeState.requestedSigShares.end();) {
            if (now - it->second >= SIG_SHARE_REQUEST_TIMEOUT) {
                // timeout while waiting for this one, so retry it with another node
                LogPrint("llmq", "CSigningManager::%s -- timeout while waiting for %s-%d, node=%d\n", __func__,
                         it->first.first.ToString(), it->first.second, nodeId);
                it = nodeState.requestedSigShares.erase(it);
            } else {
                ++it;
            }
        }

        std::map<uint256, CSigSharesInv>* invMap = nullptr;

        for (auto& p2 : nodeState.sessions) {
            auto& signHash = p2.first;
            auto& session = p2.second;

            if (recoveredSessions.count(signHash)) {
                continue;
            }

            for (size_t i = 0; i < session.announced.inv.size(); i++) {
                if (!session.announced.inv[i]) {
                    continue;
                }
                auto k = std::make_pair(signHash, (uint16_t) i);
                if (sigShares.count(k)) {
                    // we already have it
                    session.announced.inv[i] = false;
                    continue;
                }
                if (nodeState.requestedSigShares.size() >= maxRequestsForNode) {
                    // too many pending requests for this node
                    break;
                }
                bool doRequest = false;
                auto it = sigSharesRequested.find(k);
                if (it != sigSharesRequested.end()) {
                    if (now - it->second.second >= SIG_SHARE_REQUEST_TIMEOUT && nodeId != it->second.first) {
                        // other node timed out, re-request from this node
                        LogPrint("llmq", "CSigningManager::%s -- other node timeout while waiting for %s-%d, re-request from=%d, node=%d\n", __func__,
                                 it->first.first.ToString(), it->first.second, nodeId, it->second.first);
                        doRequest = true;
                    }
                } else {
                    doRequest = true;
                }

                if (doRequest) {
                    // track when we initiated the request so that we can detect timeouts
                    nodeState.requestedSigShares.emplace(k, now);

                    // don't request it from other nodes until a timeout happens
                    auto& r = sigSharesRequested[k];
                    r.first = nodeId;
                    r.second = now;

                    if (!invMap) {
                        invMap = &sigSharesToRequest[nodeId];
                    }
                    auto& inv = (*invMap)[signHash];
                    if (inv.inv.empty()) {
                        inv.Init((Consensus::LLMQType)session.announced.llmqType, signHash);
                    }
                    inv.inv[k.second] = true;

                    // dont't request it again from this node
                    session.announced.inv[i] = false;
                }
            }
        }
    }
}

// cs must be held
void CSigningManager::CollectSigSharesToSend(std::map<NodeId, std::map<uint256, CBatchedSigShares>>& sigSharesToSend)
{
    for (auto& p : nodeStates) {
        auto nodeId = p.first;
        auto& nodeState = p.second;

        std::map<uint256, CBatchedSigShares>* sigSharesToSend2 = nullptr;

        for (auto& p2 : nodeState.sessions) {
            auto& signHash = p2.first;
            auto& session = p2.second;

            if (recoveredSessions.count(signHash)) {
                continue;
            }

            CBatchedSigShares batchedSigShares;

            for (size_t i = 0; i < session.requested.inv.size(); i++) {
                if (!session.requested.inv[i]) {
                    continue;
                }
                session.requested.inv[i] = false;

                auto k = std::make_pair(signHash, (uint16_t)i);
                auto it = sigShares.find(k);
                if (it == sigShares.end()) {
                    // he requested something we don'have
                    session.requested.inv[i] = false;
                    continue;
                }

                auto& sigShare = it->second;
                if (batchedSigShares.sigShares.empty()) {
                    batchedSigShares.llmqType = sigShare.llmqType;
                    batchedSigShares.quorumHash = sigShare.quorumHash;
                    batchedSigShares.id = sigShare.id;
                    batchedSigShares.msgHash = sigShare.msgHash;
                }
                batchedSigShares.sigShares.emplace_back((uint16_t)i, sigShare.sigShare);
            }

            if (!batchedSigShares.sigShares.empty()) {
                if (sigSharesToSend2 == nullptr) {
                    // only create the map if we actually add a batched sig
                    sigSharesToSend2 = &sigSharesToSend[nodeId];
                }
                (*sigSharesToSend2).emplace(signHash, std::move(batchedSigShares));
            }
        }
    }
}

// cs must be held
void CSigningManager::CollectSigSharesToAnnounce(std::map<NodeId, std::map<uint256, CSigSharesInv>>& sigSharesToAnnounce)
{
    std::set<std::pair<Consensus::LLMQType, uint256>> quorumNodesPrepared;

    for (auto& sigShareKey : this->sigSharesToAnnounce) {
        auto& signHash = sigShareKey.first;
        auto quorumMember = sigShareKey.second;
        auto sigShareIt = sigShares.find(sigShareKey);
        if (sigShareIt == sigShares.end()) {
            continue;
        }
        auto& sigShare = sigShareIt->second;

        auto quorumKey = std::make_pair((Consensus::LLMQType)sigShare.llmqType, sigShare.quorumHash);
        if (quorumNodesPrepared.emplace(quorumKey).second) {
            // make sure we announce to at least the nodes which we know through the intra-quorum-communication system
            auto nodeIds = g_connman->GetMasternodeQuorumNodes(quorumKey.first, quorumKey.second);
            for (auto nodeId : nodeIds) {
                auto& nodeState = nodeStates[nodeId];
                nodeState.interestedIn.emplace(quorumKey);
            }
        }

        for (auto& p : nodeStates) {
            auto nodeId = p.first;
            auto& nodeState = p.second;

            if (!nodeState.interestedIn.count(quorumKey)) {
                // node is not interested in this sig share
                // we only consider nodes to be interested if they sent us valid sig share before
                // the sig share that we sign by ourself circumvents the inv system and is directly sent to all quorum members
                // which are known by the deterministic intra-quorum-communication system. This is also the sig share that
                // will tell the other nodes that we are interested in future sig shares
                continue;
            }

            auto& session = nodeState.GetOrCreateSession((Consensus::LLMQType)sigShare.llmqType, signHash);

            if (session.knows.inv[quorumMember]) {
                // he already knows that one
                continue;
            }

            auto& inv = sigSharesToAnnounce[nodeId][signHash];
            if (inv.inv.empty()) {
                inv.Init((Consensus::LLMQType)sigShare.llmqType, signHash);
            }
            inv.inv[quorumMember] = true;
            session.knows.inv[quorumMember] = true;
        }
    }

    // don't announce these anymore
    // node which did not send us a valid sig share before were left out now, but this is ok as it only results in slower
    // propagation for the first signing session of a fresh quorum. The sig shares should still arrive on all nodes due to
    // the deterministic intra-quorum-communication system
    this->sigSharesToAnnounce.clear();
}

void CSigningManager::SendMessages()
{
    std::multimap<CService, NodeId> nodesByAddress;
    g_connman->ForEachNode([&nodesByAddress](CNode* pnode) {
        nodesByAddress.emplace(pnode->addr, pnode->id);
    });

    std::map<NodeId, std::map<uint256, CSigSharesInv>> sigSharesToRequest;
    std::map<NodeId, std::map<uint256, CBatchedSigShares>> sigSharesToSend;
    std::map<NodeId, std::map<uint256, CSigSharesInv>> sigSharesToAnnounce;


    {
        LOCK(cs);
        CollectSigSharesToRequest(sigSharesToRequest);
        CollectSigSharesToSend(sigSharesToSend);
        CollectSigSharesToAnnounce(sigSharesToAnnounce);
    }

    g_connman->ForEachNode([&](CNode* pnode) {
        CNetMsgMaker msgMaker(pnode->GetSendVersion());

        auto it = sigSharesToRequest.find(pnode->id);
        if (it != sigSharesToRequest.end()) {
            for (auto& p : it->second) {
                assert(p.second.CountSet() != 0);
                LogPrint("llmq", "CSigningManager::SendMessages -- QGETSIGSHARES inv={%s}, node=%d\n",
                          p.second.ToString(), pnode->id);
                g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::QGETSIGSHARES, p.second));
            }
        }

        auto jt = sigSharesToSend.find(pnode->id);
        if (jt != sigSharesToSend.end()) {
            for (auto& p : jt->second) {
                assert(!p.second.sigShares.empty());
                LogPrint("llmq", "CSigningManager::SendMessages -- QBSIGSHARES inv={%s}, node=%d\n",
                         p.second.ToInv().ToString(), pnode->id);
                g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::QBSIGSHARES, p.second));
            }
        }

        auto kt = sigSharesToAnnounce.find(pnode->id);
        if (kt != sigSharesToAnnounce.end()) {
            for (auto& p : kt->second) {
                assert(p.second.CountSet() != 0);
                LogPrint("llmq", "CSigningManager::SendMessages -- QSIGSHARESINV inv={%s}, node=%d\n",
                         p.second.ToString(), pnode->id);
                g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::QSIGSHARESINV, p.second));
            }
        }

        return true;
    });
}

void CSigningManager::Cleanup()
{
    int64_t now = GetTimeMillis();
    if (now - lastCleanupTime < 5000) {
        return;
    }

    {
        LOCK(cs);
        std::set<SigShareKey> sigSharesToDelete;
        for (auto& p : sigShares) {
            sigSharesToDelete.emplace(p.first);
        }

        std::set<uint256> timeoutSessions;
        for (auto& p : firstSeenForSessions) {
            auto& signHash = p.first;
            int64_t time = p.second;

            if (now - time >= SIGNING_SESSION_TIMEOUT) {
                timeoutSessions.emplace(signHash);
            }
        }

        for (auto& signHash : timeoutSessions) {
            size_t count = CountBySignHash(sigShares, signHash);

            if (count > 0) {
                auto itPair = FindBySignHash(sigShares, signHash);
                auto& firstSigShare = itPair.first->second;
                LogPrintf("CSigningManager::%s -- signing session timed out. signHash=%s, id=%s, msgHash=%s, sigShareCount=%d\n", __func__,
                        signHash.ToString(), firstSigShare.id.ToString(), firstSigShare.msgHash.ToString(), count);
            } else {
                LogPrintf("CSigningManager::%s -- signing session timed out. signHash=%s, sigShareCount=%d\n", __func__,
                          signHash.ToString(), count);
            }
            RemoveSigSharesForSession(signHash);
        }
    }

    std::set<NodeId> nodeStatesToDelete;
    for (auto& p : nodeStates) {
        nodeStatesToDelete.emplace(p.first);
    }
    g_connman->ForEachNode([&](CNode* pnode) {
        nodeStatesToDelete.erase(pnode->id);
    });

    LOCK(cs);
    for (auto nodeId : nodeStatesToDelete) {
        auto& nodeState = nodeStates[nodeId];
        // remove global requested state to force a request from another node
        for (auto& p : nodeState.requestedSigShares) {
            sigSharesRequested.erase(p.first);
        }
        nodeStates.erase(nodeId);
    }

    lastCleanupTime = GetTimeMillis();
}

void CSigningManager::RemoveSigSharesForSession(const uint256& signHash)
{
    for (auto& p : nodeStates) {
        auto& ns = p.second;
        ns.Erase(signHash);
    }

    EraseBySignHash(sigSharesRequested, signHash);
    EraseBySignHash(sigSharesToAnnounce, signHash);
    EraseBySignHash(sigShares, signHash);
    firstSeenForSessions.erase(signHash);
}

void CSigningManager::RemoveBannedNodeStates()
{
    LOCK2(cs_main, cs);
    std::set<NodeId> toRemove;
    for (auto it = nodeStates.begin(); it != nodeStates.end();) {
        if (IsBanned(it->first)) {
            it = nodeStates.erase(it);
        } else {
            ++it;
        }
    }
}

void CSigningManager::WorkThreadMain()
{
    int64_t lastProcessTime = GetTimeMillis();
    while (!stopWorkThread && !ShutdownRequested()) {
        if (GetTimeMillis() - lastProcessTime >= 100) {
            RemoveBannedNodeStates();
            ProcessPendingIncomingSigs(*g_connman);
            SendMessages();
            Cleanup();
            lastProcessTime = GetTimeMillis();
        }

        int64_t now = GetTimeMillis();

        std::list<WorkQueueItem> jobs;
        {
            std::unique_lock<std::mutex> l(workQueueMutex);
            for (auto it = workQueue.begin(); it != workQueue.end();) {
                auto& job = *it;
                if (now >= job.at) {
                    jobs.emplace_back(std::move(job));
                    it = workQueue.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto& job : jobs) {
            if (stopWorkThread || ShutdownRequested()) {
                break;
            }
            job.func();
        }

        MilliSleep(100);
    }
}

bool CSigningManager::HasRecoveredSig(Consensus::LLMQType llmqType, const uint256& id, const uint256& msgHash)
{
    CRecoveredSig recoveredSig;
    {
        LOCK(cs);
        auto it = recoveredSigsForIds.find(std::make_pair(llmqType, id));
        if (it == recoveredSigsForIds.end()) {
            return false;
        }
        auto it2 = recoveredSigs.find(it->second);
        if (it2 == recoveredSigs.end()) {
            // should not happen
            return false;
        }
        recoveredSig = it2->second;
        if (recoveredSig.msgHash != msgHash) {
            // conflicting
            return false;
        }
    }

    LOCK(cs_main);
    if (!IsQuorumActive(llmqType, recoveredSig.quorumHash)) {
        return false;
    }
    return true;
}

bool CSigningManager::IsConflicting(Consensus::LLMQType llmqType, const uint256& id, const uint256& msgHash)
{
    CRecoveredSig recoveredSig;
    {
        LOCK(cs);
        auto it = recoveredSigsForIds.find(std::make_pair(llmqType, id));
        if (it == recoveredSigsForIds.end()) {
            return false;
        }
        auto it2 = recoveredSigs.find(it->second);
        if (it2 == recoveredSigs.end()) {
            // should not happen (signal conflict, even thought it's technically not a conflict. makes sure we don't accept something because there is a bug somewhere here)
            return true;
        }
        recoveredSig = it2->second;
    }

    LOCK(cs_main);
    if (!IsQuorumActive(llmqType, recoveredSig.quorumHash)) {
        return false;
    }
    return recoveredSig.msgHash != msgHash;
}

void CSigShare::UpdateKey()
{
    key.first = MakeSignHash(*this);
    key.second = quorumMember;
}

void CSigSharesInv::Merge(const llmq::CSigSharesInv& inv2)
{
    assert(llmqType == inv2.llmqType);
    assert(signHash == inv2.signHash);
    for (size_t i = 0; i < inv.size(); i++) {
        if (inv2.inv[i]) {
            inv[i] = inv2.inv[i];
        }
    }
}

size_t CSigSharesInv::CountSet() const
{
    return std::count(inv.begin(), inv.end(), true);
}

std::string CSigSharesInv::ToString() const
{
    std::string str = strprintf("signHash=%s, inv=(", signHash.ToString());
    bool first = true;
    for (size_t i = 0; i < inv.size(); i++) {
        if (!inv[i]) {
            continue;
        }

        if (!first) {
            str += ",";
        }
        first = false;
        str += strprintf("%d", i);
    }
    str += ")";
    return str;
}

void CSigSharesInv::Init(Consensus::LLMQType _llmqType, const uint256& _signHash)
{
    llmqType = _llmqType;
    signHash = _signHash;

    size_t llmqSize = (size_t)(Params().GetConsensus().llmqs.at(_llmqType).size);
    inv.resize(llmqSize, false);
}

bool CSigSharesInv::IsMarked(uint16_t quorumMember) const
{
    assert(quorumMember < inv.size());
    return inv[quorumMember];
}

void CSigSharesInv::Set(uint16_t quorumMember, bool v)
{
    assert(quorumMember < inv.size());
    inv[quorumMember] = v;
}

CSigSharesNodeState::Session& CSigSharesNodeState::GetOrCreateSession(Consensus::LLMQType llmqType, const uint256& signHash)
{
    auto& s = sessions[signHash];
    if (s.announced.inv.empty()) {
        s.announced.Init(llmqType, signHash);
        s.requested.Init(llmqType, signHash);
        s.knows.Init(llmqType, signHash);
    } else {
        assert(s.announced.llmqType == llmqType);
        assert(s.requested.llmqType == llmqType);
        assert(s.knows.llmqType == llmqType);
    }
    return s;
}

void CSigSharesNodeState::MarkAnnounced(const uint256& signHash, const CSigSharesInv& inv)
{
    GetOrCreateSession((Consensus::LLMQType)inv.llmqType, signHash).announced.Merge(inv);
}

void CSigSharesNodeState::MarkRequested(const uint256& signHash, const CSigSharesInv& inv)
{
    GetOrCreateSession((Consensus::LLMQType)inv.llmqType, signHash).requested.Merge(inv);
}

void CSigSharesNodeState::MarkKnows(const uint256& signHash, const CSigSharesInv& inv)
{
    GetOrCreateSession((Consensus::LLMQType)inv.llmqType, signHash).knows.Merge(inv);
}

void CSigSharesNodeState::MarkAnnounced(Consensus::LLMQType llmqType, const uint256& signHash, uint16_t quorumMember)
{
    GetOrCreateSession(llmqType, signHash).announced.Set(quorumMember, true);
}

void CSigSharesNodeState::MarkRequested(Consensus::LLMQType llmqType, const uint256& signHash, uint16_t quorumMember)
{
    GetOrCreateSession(llmqType, signHash).requested.Set(quorumMember, true);
}

void CSigSharesNodeState::MarkKnows(Consensus::LLMQType llmqType, const uint256& signHash, uint16_t quorumMember)
{
    GetOrCreateSession(llmqType, signHash).knows.Set(quorumMember, true);
}

bool CSigSharesNodeState::Announced(const uint256& signHash, uint16_t quorumMember) const
{
    auto it = sessions.find(signHash);
    if (it == sessions.end()) {
        return false;
    }
    return it->second.announced.IsMarked(quorumMember);
}

bool CSigSharesNodeState::Requested(const uint256& signHash, uint16_t quorumMember) const
{
    auto it = sessions.find(signHash);
    if (it == sessions.end()) {
        return false;
    }
    return it->second.requested.IsMarked(quorumMember);
}

bool CSigSharesNodeState::Knows(const uint256& signHash, uint16_t quorumMember) const
{
    auto it = sessions.find(signHash);
    if (it == sessions.end()) {
        return false;
    }
    return it->second.knows.IsMarked(quorumMember);
}

void CSigSharesNodeState::Erase(const uint256& signHash, uint16_t quorumMember)
{
    auto it = sessions.find(signHash);
    if (it == sessions.end()) {
        return;
    }
    auto& s = it->second;
    s.announced.Set(quorumMember, false);
    s.requested.Set(quorumMember, false);
    s.knows.Set(quorumMember, false);

    bool anySet = false;
    for (size_t i = 0; i < s.announced.inv.size(); i++) {
        if (s.announced.inv[i] || s.requested.inv[i] || s.knows.inv[i]) {
            anySet = true;
            break;
        }
    }
    if (!anySet) {
        sessions.erase(it);
    }
}

void CSigSharesNodeState::Erase(const uint256& signHash)
{
    sessions.erase(signHash);
    pendingIncomingRecSigs.erase(signHash);
    EraseBySignHash(requestedSigShares, signHash);
    EraseBySignHash(pendingIncomingSigShares, signHash);
}

CSigSharesInv CBatchedSigShares::ToInv() const
{
    CSigSharesInv inv;
    inv.Init((Consensus::LLMQType)llmqType, MakeSignHash(*this));
    for (size_t i = 0; i < sigShares.size(); i++) {
        inv.inv[sigShares[i].first] = true;
    }
    return inv;
}

}
