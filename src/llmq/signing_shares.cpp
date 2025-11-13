// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/signing_shares.h>

#include <llmq/commitment.h>
#include <llmq/options.h>
#include <llmq/quorums.h>
#include <llmq/signhash.h>
#include <llmq/signing.h>

#include <bls/bls_batchverifier.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <masternode/node.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <spork.h>
#include <util/irange.h>
#include <util/thread.h>
#include <util/time.h>
#include <util/underlying.h>
#include <validation.h>

#include <cxxtimer.hpp>

namespace llmq
{
void CSigShare::UpdateKey()
{
    key.first = this->buildSignHash().Get();
    key.second = quorumMember;
}

void CSigSharesInv::Merge(const CSigSharesInv& inv2)
{
    for (const auto i : irange::range(inv.size())) {
        if (inv2.inv[i]) {
            inv[i] = inv2.inv[i];
        }
    }
}

size_t CSigSharesInv::CountSet() const
{
    return (size_t)std::count(inv.begin(), inv.end(), true);
}

std::string CSigSharesInv::ToString() const
{
    std::string str = "(";
    bool first = true;
    for (const auto i : irange::range(inv.size())) {
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

void CSigSharesInv::Init(size_t size)
{
    inv.resize(size, false);
}

void CSigSharesInv::Set(uint16_t quorumMember, bool v)
{
    assert(quorumMember < inv.size());
    inv[quorumMember] = v;
}

void CSigSharesInv::SetAll(bool v)
{
    std::fill(inv.begin(), inv.end(), v);
}

void CSigSharesNodeState::RemoveSession(const uint256& signHash)
{
    if (const auto it = sessions.find(signHash); it != sessions.end()) {
        sessions.erase(it);
    }
    requestedSigShares.EraseAllForSignHash(signHash);
    pendingIncomingSigShares.EraseAllForSignHash(signHash);
}

//////////////////////

CSigSharesManager::CSigSharesManager(CConnman& connman, CChainState& chainstate, CSigningManager& _sigman,
                                     PeerManager& peerman, const CActiveMasternodeManager& mn_activeman,
                                     const CQuorumManager& _qman, const CSporkManager& sporkman) :
    m_connman{connman},
    m_chainstate{chainstate},
    sigman{_sigman},
    m_peerman{peerman},
    m_mn_activeman{mn_activeman},
    qman{_qman},
    m_sporkman{sporkman}
{
    workInterrupt.reset();
}

CSigSharesManager::~CSigSharesManager() = default;

void CSigSharesManager::StartWorkerThread()
{
    // can't start new thread if we have one running already
    if (workThread.joinable()) {
        assert(false);
    }

    workThread = std::thread(&util::TraceThread, "sigshares", [this] { WorkThreadMain(); });
}

void CSigSharesManager::StopWorkerThread()
{
    // make sure to call InterruptWorkerThread() first
    if (!workInterrupt) {
        assert(false);
    }

    if (workThread.joinable()) {
        workThread.join();
    }
}

void CSigSharesManager::RegisterAsRecoveredSigsListener()
{
    sigman.RegisterRecoveredSigsListener(this);
}

void CSigSharesManager::UnregisterAsRecoveredSigsListener()
{
    sigman.UnregisterRecoveredSigsListener(this);
}

void CSigSharesManager::InterruptWorkerThread()
{
    workInterrupt();
}

void CSigSharesManager::ProcessMessage(const CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    // non-masternodes are not interested in sigshares
    if (m_mn_activeman.GetProTxHash().IsNull()) return;

    if (msg_type == NetMsgType::QSIGSHARE) {
        std::vector<CSigShare> receivedSigShares;
        vRecv >> receivedSigShares;

        if (receivedSigShares.size() > MAX_MSGS_SIG_SHARES) {
            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- too many sigs in QSIGSHARE message. cnt=%d, max=%d, node=%d\n", __func__, receivedSigShares.size(), MAX_MSGS_SIG_SHARES, pfrom.GetId());
            BanNode(pfrom.GetId());
            return;
        }

        for (const auto& sigShare : receivedSigShares) {
            ProcessMessageSigShare(pfrom.GetId(), sigShare);
        }
    } else if (msg_type == NetMsgType::QSIGSESANN) {
        return; // Do nothing: this message is not expected to be received once spork21 is hardened as active
    } else if (msg_type == NetMsgType::QSIGSHARESINV) {
        return; // Do nothing: this message is not expected to be received once spork21 is hardened as active
    } else if (msg_type == NetMsgType::QGETSIGSHARES) {
        return; // Do nothing: this message is not expected to be received once spork21 is hardened as active
    } else if (msg_type == NetMsgType::QBSIGSHARES) {
        return; // Do nothing: this message is not expected to be received once spork21 is hardened as active
    }
}

void CSigSharesManager::ProcessMessageSigShare(NodeId fromId, const CSigShare& sigShare)
{
    auto quorum = qman.GetQuorum(sigShare.getLlmqType(), sigShare.getQuorumHash());
    if (!quorum) {
        return;
    }
    if (!IsQuorumActive(sigShare.getLlmqType(), qman, quorum->qc->quorumHash)) {
        // quorum is too old
        return;
    }
    if (!quorum->IsMember(m_mn_activeman.GetProTxHash())) {
        // we're not a member so we can't verify it (we actually shouldn't have received it)
        return;
    }
    if (!quorum->HasVerificationVector()) {
        // TODO we should allow to ask other nodes for the quorum vvec if we missed it in the DKG
        LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- we don't have the quorum vvec for %s, no verification possible. node=%d\n", __func__,
                 quorum->qc->quorumHash.ToString(), fromId);
        return;
    }

    if (sigShare.getQuorumMember() >= quorum->members.size()) {
        LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- quorumMember out of bounds\n", __func__);
        BanNode(fromId);
        return;
    }
    if (!quorum->qc->validMembers[sigShare.getQuorumMember()]) {
        LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- quorumMember not valid\n", __func__);
        BanNode(fromId);
        return;
    }

    {
        LOCK(cs);

        if (sigShares.Has(sigShare.GetKey())) {
            return;
        }

        if (sigman.HasRecoveredSigForId(sigShare.getLlmqType(), sigShare.getId())) {
            return;
        }

        auto& nodeState = nodeStates[fromId];
        nodeState.pendingIncomingSigShares.Add(sigShare.GetKey(), sigShare);
    }

    LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- signHash=%s, id=%s, msgHash=%s, member=%d, node=%d\n", __func__,
             sigShare.GetSignHash().ToString(), sigShare.getId().ToString(), sigShare.getMsgHash().ToString(), sigShare.getQuorumMember(), fromId);
}

bool CSigSharesManager::CollectPendingSigSharesToVerify(
    size_t maxUniqueSessions, std::unordered_map<NodeId, std::vector<CSigShare>>& retSigShares,
    std::unordered_map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr, StaticSaltedHasher>& retQuorums)
{
    {
        LOCK(cs);
        if (nodeStates.empty()) {
            return false;
        }

        // This will iterate node states in random order and pick one sig share at a time. This avoids processing
        // of large batches at once from the same node while other nodes also provided shares. If we wouldn't do this,
        // other nodes would be able to poison us with a large batch with N-1 valid shares and the last one being
        // invalid, making batch verification fail and revert to per-share verification, which in turn would slow down
        // the whole verification process
        std::unordered_set<std::pair<NodeId, uint256>, StaticSaltedHasher> uniqueSignHashes;
        IterateNodesRandom(
            nodeStates,
            [&]() {
                return uniqueSignHashes.size() < maxUniqueSessions;
                // TODO: remove NO_THREAD_SAFETY_ANALYSIS
                // using here template IterateNodesRandom makes impossible to use lock annotation
            },
            [&](NodeId nodeId, CSigSharesNodeState& ns) NO_THREAD_SAFETY_ANALYSIS {
                if (ns.pendingIncomingSigShares.Empty()) {
                    return false;
                }
                const auto& sigShare = *ns.pendingIncomingSigShares.GetFirst();

                AssertLockHeld(cs);
                if (const bool alreadyHave = this->sigShares.Has(sigShare.GetKey()); !alreadyHave) {
                    uniqueSignHashes.emplace(nodeId, sigShare.GetSignHash());
                    retSigShares[nodeId].emplace_back(sigShare);
                }
                ns.pendingIncomingSigShares.Erase(sigShare.GetKey());
                return !ns.pendingIncomingSigShares.Empty();
            },
            rnd);

        if (retSigShares.empty()) {
            return false;
        }
    }

    // For the convenience of the caller, also build a map of quorumHash -> quorum

    for (const auto& [_, vecSigShares] : retSigShares) {
        for (const auto& sigShare : vecSigShares) {
            auto llmqType = sigShare.getLlmqType();

            auto k = std::make_pair(llmqType, sigShare.getQuorumHash());
            if (retQuorums.count(k) != 0) {
                continue;
            }

            auto quorum = qman.GetQuorum(llmqType, sigShare.getQuorumHash());
            // Despite constructing a convenience map, we assume that the quorum *must* be present.
            // The absence of it might indicate an inconsistent internal state, so we should report
            // nothing instead of reporting flawed data.
            if (!quorum) {
                LogPrintf("%s: ERROR! Unexpected missing quorum with llmqType=%d, quorumHash=%s\n", __func__,
                          ToUnderlying(llmqType), sigShare.getQuorumHash().ToString());
                return false;
            }
            retQuorums.try_emplace(k, quorum);
        }
    }

    return true;
}

bool CSigSharesManager::ProcessPendingSigShares()
{
    std::unordered_map<NodeId, std::vector<CSigShare>> sigSharesByNodes;
    std::unordered_map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr, StaticSaltedHasher> quorums;

    const size_t nMaxBatchSize{32};
    bool collect_status = CollectPendingSigSharesToVerify(nMaxBatchSize, sigSharesByNodes, quorums);
    if (!collect_status || sigSharesByNodes.empty()) {
        return false;
    }

    // It's ok to perform insecure batched verification here as we verify against the quorum public key shares,
    // which are not craftable by individual entities, making the rogue public key attack impossible
    CBLSBatchVerifier<NodeId, SigShareKey> batchVerifier(false, true);

    cxxtimer::Timer prepareTimer(true);
    size_t verifyCount = 0;
    for (const auto& [nodeId, v] : sigSharesByNodes) {
        for (const auto& sigShare : v) {
            if (sigman.HasRecoveredSigForId(sigShare.getLlmqType(), sigShare.getId())) {
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
                LogPrintf("CSigSharesManager::%s -- pubKeyShare is invalid, which should not be possible here\n", __func__);
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

    LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- verified sig shares. count=%d, pt=%d, vt=%d, nodes=%d\n", __func__, verifyCount, prepareTimer.count(), verifyTimer.count(), sigSharesByNodes.size());

    for (const auto& [nodeId, v] : sigSharesByNodes) {
        if (batchVerifier.badSources.count(nodeId) != 0) {
            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- invalid sig shares from other node, banning peer=%d\n",
                     __func__, nodeId);
            // this will also cause re-requesting of the shares that were sent by this node
            BanNode(nodeId);
            continue;
        }

        ProcessPendingSigShares(v, quorums);
    }

    return sigSharesByNodes.size() >= nMaxBatchSize;
}

// It's ensured that no duplicates are passed to this method
void CSigSharesManager::ProcessPendingSigShares(
    const std::vector<CSigShare>& sigSharesToProcess,
    const std::unordered_map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr, StaticSaltedHasher>& quorums)
{
    cxxtimer::Timer t(true);
    for (const auto& sigShare : sigSharesToProcess) {
        auto quorumKey = std::make_pair(sigShare.getLlmqType(), sigShare.getQuorumHash());
        ProcessSigShare(sigShare, quorums.at(quorumKey));
    }
    t.stop();

    LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- processed sigShare batch. shares=%d, time=%ds\n", __func__,
             sigSharesToProcess.size(), t.count());
}

// sig shares are already verified when entering this method
void CSigSharesManager::ProcessSigShare(const CSigShare& sigShare, const CQuorumCPtr& quorum)
{
    auto llmqType = quorum->params.type;
    bool canTryRecovery = false;

    const bool isAllMembersConnectedEnabled = IsAllMembersConnectedEnabled(llmqType, m_sporkman);

    // prepare node set for direct-push in case this is our sig share
    std::vector<NodeId> quorumNodes;
    if (!isAllMembersConnectedEnabled &&
        sigShare.getQuorumMember() == quorum->GetMemberIndex(m_mn_activeman.GetProTxHash())) {
        quorumNodes = m_connman.GetMasternodeQuorumNodes(sigShare.getLlmqType(), sigShare.getQuorumHash());
    }

    if (sigman.HasRecoveredSigForId(llmqType, sigShare.getId())) {
        return;
    }

    {
        LOCK(cs);

        if (!sigShares.Add(sigShare.GetKey(), sigShare)) {
            return;
        }

        // Update the time we've seen the last sigShare
        timeSeenForSessions[sigShare.GetSignHash()] = GetTime<std::chrono::seconds>().count();

        // don't announce and wait for other nodes to request this share and directly send it to them
        // there is no way the other nodes know about this share as this is the one created on this node
        for (auto _: quorumNodes) {
            // quorumNodes is always empty because isAllMembersConnectedEnabled is always true
        }

        size_t sigShareCount = sigShares.CountForSignHash(sigShare.GetSignHash());
        if (sigShareCount >= size_t(quorum->params.threshold)) {
            canTryRecovery = true;
        }
    }

    if (canTryRecovery) {
        TryRecoverSig(*quorum, sigShare.getId(), sigShare.getMsgHash());
    }
}

void CSigSharesManager::TryRecoverSig(const CQuorum& quorum, const uint256& id, const uint256& msgHash)
{
    if (sigman.HasRecoveredSigForId(quorum.params.type, id)) {
        return;
    }

    std::vector<CBLSSignature> sigSharesForRecovery;
    std::vector<CBLSId> idsForRecovery;
    std::shared_ptr<CRecoveredSig> singleMemberRecoveredSig;
    {
        LOCK(cs);

        auto signHash = SignHash(quorum.params.type, quorum.qc->quorumHash, id, msgHash).Get();
        const auto* sigSharesForSignHash = sigShares.GetAllForSignHash(signHash);
        if (sigSharesForSignHash == nullptr) {
            return;
        }

        if (quorum.params.is_single_member()) {
            if (sigSharesForSignHash->empty()) {
                LogPrint(BCLog::LLMQ_SIGS, /* Continued */
                         "CSigSharesManager::%s -- impossible to recover single-node signature - no shares yet. id=%s, "
                         "msgHash=%s\n",
                         __func__, id.ToString(), msgHash.ToString());
                return;
            }
            const auto& sigShare = sigSharesForSignHash->begin()->second;
            CBLSSignature recoveredSig = sigShare.sigShare.Get();
            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- recover single-node signature. id=%s, msgHash=%s\n",
                     __func__, id.ToString(), msgHash.ToString());

            singleMemberRecoveredSig = std::make_shared<CRecoveredSig>(quorum.params.type, quorum.qc->quorumHash, id, msgHash,
                                                      recoveredSig);
        }

        sigSharesForRecovery.reserve((size_t) quorum.params.threshold);
        idsForRecovery.reserve((size_t) quorum.params.threshold);
        for (auto it = sigSharesForSignHash->begin(); it != sigSharesForSignHash->end() && sigSharesForRecovery.size() < size_t(quorum.params.threshold); ++it) {
            const auto& sigShare = it->second;
            sigSharesForRecovery.emplace_back(sigShare.sigShare.Get());
            idsForRecovery.emplace_back(quorum.members[sigShare.getQuorumMember()]->proTxHash);
        }

        // check if we can recover the final signature
        if (sigSharesForRecovery.size() < size_t(quorum.params.threshold)) {
            return;
        }
    }

    // Handle single-member quorum case after releasing the lock
    if (singleMemberRecoveredSig) {
        sigman.ProcessRecoveredSig(singleMemberRecoveredSig, m_peerman);
        return; // end of single-quorum processing
    }

    // now recover it
    cxxtimer::Timer t(true);
    CBLSSignature recoveredSig;
    if (!recoveredSig.Recover(sigSharesForRecovery, idsForRecovery)) {
        LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- failed to recover signature. id=%s, msgHash=%s, time=%d\n", __func__,
                  id.ToString(), msgHash.ToString(), t.count());
        return;
    }

    LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- recovered signature. id=%s, msgHash=%s, time=%d\n", __func__,
              id.ToString(), msgHash.ToString(), t.count());

    auto rs = std::make_shared<CRecoveredSig>(quorum.params.type, quorum.qc->quorumHash, id, msgHash, recoveredSig);

    // There should actually be no need to verify the self-recovered signatures as it should always succeed. Let's
    // however still verify it from time to time, so that we have a chance to catch bugs. We do only this sporadic
    // verification because this is unbatched and thus slow verification that happens here.
    if (((recoveredSigsCounter++) % 100) == 0) {
        auto signHash = rs->buildSignHash();
        bool valid = recoveredSig.VerifyInsecure(quorum.qc->quorumPublicKey, signHash.Get());
        if (!valid) {
            // this should really not happen as we have verified all signature shares before
            LogPrintf("CSigSharesManager::%s -- own recovered signature is invalid. id=%s, msgHash=%s\n", __func__,
                      id.ToString(), msgHash.ToString());
            return;
        }
    }

    sigman.ProcessRecoveredSig(rs, m_peerman);
}

CDeterministicMNCPtr CSigSharesManager::SelectMemberForRecovery(const CQuorum& quorum, const uint256 &id, int attempt)
{
    assert(attempt < quorum.params.recoveryMembers);

    std::vector<std::pair<uint256, CDeterministicMNCPtr>> v;
    v.reserve(quorum.members.size());
    for (const auto& dmn : quorum.members) {
        auto h = ::SerializeHash(std::make_pair(dmn->proTxHash, id));
        v.emplace_back(h, dmn);
    }
    std::sort(v.begin(), v.end());

    return v[attempt % v.size()].second;
}

bool CSigSharesManager::AsyncSignIfMember(Consensus::LLMQType llmqType, CSigningManager& sigman, const uint256& id,
                                          const uint256& msgHash, const uint256& quorumHash, bool allowReSign,
                                          bool allowDiffMsgHashSigning)
{
    AssertLockNotHeld(cs_pendingSigns);

    if (m_mn_activeman.GetProTxHash().IsNull()) return false;

    auto quorum = [&]() {
        if (quorumHash.IsNull()) {
            // This might end up giving different results on different members
            // This might happen when we are on the brink of confirming a new quorum
            // This gives a slight risk of not getting enough shares to recover a signature
            // But at least it shouldn't be possible to get conflicting recovered signatures
            // TODO fix this by re-signing when the next block arrives, but only when that block results in a change of
            // the quorum list and no recovered signature has been created in the mean time
            const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
            assert(llmq_params_opt.has_value());
            return SelectQuorumForSigning(llmq_params_opt.value(), m_chainstate.m_chain, qman, id);
        } else {
            return qman.GetQuorum(llmqType, quorumHash);
        }
    }();

    if (!quorum) {
        LogPrint(BCLog::LLMQ, "CSigningManager::%s -- failed to select quorum. id=%s, msgHash=%s\n", __func__,
                 id.ToString(), msgHash.ToString());
        return false;
    }

    if (!quorum->IsValidMember(m_mn_activeman.GetProTxHash())) {
        return false;
    }

    {
        auto& db = sigman.GetDb();
        bool hasVoted = db.HasVotedOnId(llmqType, id);
        if (hasVoted) {
            uint256 prevMsgHash;
            db.GetVoteForId(llmqType, id, prevMsgHash);
            if (msgHash != prevMsgHash) {
                if (allowDiffMsgHashSigning) {
                    LogPrintf("%s -- already voted for id=%s and msgHash=%s. Signing for different " /* Continued */
                              "msgHash=%s\n",
                              __func__, id.ToString(), prevMsgHash.ToString(), msgHash.ToString());
                    hasVoted = false;
                } else {
                    LogPrintf("%s -- already voted for id=%s and msgHash=%s. Not voting on " /* Continued */
                              "conflicting msgHash=%s\n",
                              __func__, id.ToString(), prevMsgHash.ToString(), msgHash.ToString());
                    return false;
                }
            } else if (allowReSign) {
                LogPrint(BCLog::LLMQ, "%s -- already voted for id=%s and msgHash=%s. Resigning!\n", __func__,
                         id.ToString(), prevMsgHash.ToString());
            } else {
                LogPrint(BCLog::LLMQ, "%s -- already voted for id=%s and msgHash=%s. Not voting again.\n", __func__,
                         id.ToString(), prevMsgHash.ToString());
                return false;
            }
        }

        if (db.HasRecoveredSigForId(llmqType, id)) {
            // no need to sign it if we already have a recovered sig
            return true;
        }
        if (!hasVoted) {
            db.WriteVoteForId(llmqType, id, msgHash);
        }
    }

    AsyncSign(std::move(quorum), id, msgHash);

    return true;
}

void CSigSharesManager::NotifyRecoveredSig(const std::shared_ptr<const CRecoveredSig>& sig) const
{
    m_peerman.RelayRecoveredSig(Assert(sig)->GetHash());
}

void CSigSharesManager::CollectSigSharesToSendConcentrated(std::unordered_map<NodeId, std::vector<CSigShare>>& sigSharesToSend, const std::vector<CNode*>& vNodes)
{
    AssertLockHeld(cs);

    Uint256HashMap<CNode*> proTxToNode;
    for (const auto& pnode : vNodes) {
        auto verifiedProRegTxHash = pnode->GetVerifiedProRegTxHash();
        if (verifiedProRegTxHash.IsNull()) {
            continue;
        }
        proTxToNode.try_emplace(verifiedProRegTxHash, pnode);
    }

    auto curTime = GetTime<std::chrono::milliseconds>().count();

    for (auto& [_, signedSession] : signedSessions) {
        if (!IsAllMembersConnectedEnabled(signedSession.quorum->params.type, m_sporkman)) {
            continue;
        }

        if (signedSession.attempt >= signedSession.quorum->params.recoveryMembers) {
            continue;
        }

        if (curTime >= signedSession.nextAttemptTime) {
            int64_t waitTime = exp2(signedSession.attempt) * EXP_SEND_FOR_RECOVERY_TIMEOUT;
            waitTime = std::min(MAX_SEND_FOR_RECOVERY_TIMEOUT, waitTime);
            signedSession.nextAttemptTime = curTime + waitTime;
            auto dmn = SelectMemberForRecovery(*signedSession.quorum, signedSession.sigShare.getId(), signedSession.attempt);
            signedSession.attempt++;

            LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- signHash=%s, sending to %s, attempt=%d\n", __func__,
                     signedSession.sigShare.GetSignHash().ToString(), dmn->proTxHash.ToString(), signedSession.attempt);

            auto it = proTxToNode.find(dmn->proTxHash);
            if (it == proTxToNode.end()) {
                continue;
            }

            auto& m = sigSharesToSend[it->second->GetId()];
            m.emplace_back(signedSession.sigShare);
        }
    }
}

bool CSigSharesManager::SendMessages()
{
    std::unordered_map<NodeId, std::vector<CSigShare>> sigSharesToSend;

    const CConnman::NodesSnapshot snap{m_connman, /* cond = */ CConnman::FullyConnectedOnly};
    {
        LOCK(cs);
        CollectSigSharesToSendConcentrated(sigSharesToSend, snap.Nodes());
    }

    bool didSend = false;

    for (auto& pnode : snap.Nodes()) {
        CNetMsgMaker msgMaker(pnode->GetCommonVersion());

        auto lt = sigSharesToSend.find(pnode->GetId());
        if (lt != sigSharesToSend.end()) {
            std::vector<CSigShare> msgs;
            for (auto& sigShare : lt->second) {
                LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::SendMessages -- QSIGSHARE signHash=%s, node=%d\n",
                         sigShare.GetSignHash().ToString(), pnode->GetId());
                msgs.emplace_back(std::move(sigShare));
                if (msgs.size() == MAX_MSGS_SIG_SHARES) {
                    m_connman.PushMessage(pnode, msgMaker.Make(NetMsgType::QSIGSHARE, msgs));
                    msgs.clear();
                    didSend = true;
                }
            }
            if (!msgs.empty()) {
                m_connman.PushMessage(pnode, msgMaker.Make(NetMsgType::QSIGSHARE, msgs));
                didSend = true;
            }
        }
    }

    return didSend;
}

void CSigSharesManager::Cleanup()
{
    int64_t now = GetTime<std::chrono::seconds>().count();
    if (now - lastCleanupTime < 5) {
        return;
    }

    // This map is first filled with all quorums found in all sig shares. Then we remove all inactive quorums and
    // loop through all sig shares again to find the ones belonging to the inactive quorums. We then delete the
    // sessions belonging to the sig shares. At the same time, we use this map as a cache when we later need to resolve
    // quorumHash -> quorumPtr (as GetQuorum() requires cs_main, leading to deadlocks with cs held)
    std::unordered_map<std::pair<Consensus::LLMQType, uint256>, CQuorumCPtr, StaticSaltedHasher> quorums;

    {
        LOCK(cs);
        sigShares.ForEach([&quorums](const SigShareKey&, const CSigShare& sigShare) {
            quorums.try_emplace(std::make_pair(sigShare.getLlmqType(), sigShare.getQuorumHash()), nullptr);
        });
    }

    // Find quorums which became inactive
    for (auto it = quorums.begin(); it != quorums.end(); ) {
        if (IsQuorumActive(it->first.first, qman, it->first.second)) {
            auto quorum = qman.GetQuorum(it->first.first, it->first.second);
            if (quorum) {
                it->second = quorum;
                ++it;
                continue;
            }
        }
        it = quorums.erase(it);
    }

    {
        // Now delete sessions which are for inactive quorums
        LOCK(cs);
        Uint256HashSet inactiveQuorumSessions;
        sigShares.ForEach([&quorums, &inactiveQuorumSessions](const SigShareKey&, const CSigShare& sigShare) {
            if (quorums.count(std::make_pair(sigShare.getLlmqType(), sigShare.getQuorumHash())) == 0) {
                inactiveQuorumSessions.emplace(sigShare.GetSignHash());
            }
        });
        for (const auto& signHash : inactiveQuorumSessions) {
            RemoveSigSharesForSession(signHash);
        }
    }

    {
        LOCK(cs);

        // Remove sessions which were successfully recovered
        Uint256HashSet doneSessions;
        sigShares.ForEach([&doneSessions, this](const SigShareKey&, const CSigShare& sigShare) {
            if (doneSessions.count(sigShare.GetSignHash()) != 0) {
                return;
            }
            if (sigman.HasRecoveredSigForSession(sigShare.GetSignHash())) {
                doneSessions.emplace(sigShare.GetSignHash());
            }
        });
        for (const auto& signHash : doneSessions) {
            RemoveSigSharesForSession(signHash);
        }

        // Remove sessions which timed out
        Uint256HashSet timeoutSessions;
        for (const auto& [signHash, lastSeenTime] : timeSeenForSessions) {
            if (now - lastSeenTime >= SESSION_NEW_SHARES_TIMEOUT) {
                timeoutSessions.emplace(signHash);
            }
        }
        for (const auto& signHash : timeoutSessions) {

            if (const size_t count = sigShares.CountForSignHash(signHash); count > 0) {
                const auto* m = sigShares.GetAllForSignHash(signHash);
                assert(m);

                const auto& oneSigShare = m->begin()->second;

                std::string strMissingMembers;
                if (LogAcceptDebug(BCLog::LLMQ_SIGS)) {
                    if (const auto quorumIt = quorums.find(std::make_pair(oneSigShare.getLlmqType(), oneSigShare.getQuorumHash())); quorumIt != quorums.end()) {
                        const auto& quorum = quorumIt->second;
                        for (const auto i : irange::range(quorum->members.size())) {
                            if (m->count((uint16_t)i) == 0) {
                                const auto& dmn = quorum->members[i];
                                strMissingMembers += strprintf("\n  %s", dmn->proTxHash.ToString());
                            }
                        }
                    }
                }

                LogPrintLevel(BCLog::LLMQ_SIGS, BCLog::Level::Info, /* Continued */
                              "CSigSharesManager::%s -- signing session timed out. signHash=%s, id=%s, msgHash=%s, "
                              "sigShareCount=%d, missingMembers=%s\n",
                              __func__, signHash.ToString(), oneSigShare.getId().ToString(),
                              oneSigShare.getMsgHash().ToString(), count, strMissingMembers);
            } else {
                LogPrintLevel(BCLog::LLMQ_SIGS, BCLog::Level::Info, /* Continued */
                              "CSigSharesManager::%s -- signing session timed out. signHash=%s, sigShareCount=%d\n",
                              __func__, signHash.ToString(), count);
            }
            RemoveSigSharesForSession(signHash);
        }
    }

    // Find node states for peers that disappeared from CConnman
    std::unordered_set<NodeId> nodeStatesToDelete;
    {
        LOCK(cs);
        for (const auto& [nodeId, _] : nodeStates) {
            nodeStatesToDelete.emplace(nodeId);
        }
    }
    m_connman.ForEachNode([&nodeStatesToDelete](const CNode* pnode) { nodeStatesToDelete.erase(pnode->GetId()); });

    // Now delete these node states
    LOCK(cs);
    for (const auto& nodeId : nodeStatesToDelete) {
        auto it = nodeStates.find(nodeId);
        if (it == nodeStates.end()) {
            continue;
        }
        nodeStates.erase(nodeId);
    }

    lastCleanupTime = GetTime<std::chrono::seconds>().count();
}

void CSigSharesManager::RemoveSigSharesForSession(const uint256& signHash)
{
    AssertLockHeld(cs);

    for (auto& [_, nodeState] : nodeStates) {
        nodeState.RemoveSession(signHash);
    }

    sigShares.EraseAllForSignHash(signHash);
    signedSessions.erase(signHash);
    timeSeenForSessions.erase(signHash);
}

void CSigSharesManager::RemoveBannedNodeStates()
{
    // Called regularly to cleanup local node states for banned nodes

    LOCK(cs);
    for (auto it = nodeStates.begin(); it != nodeStates.end();) {
        if (m_peerman.IsBanned(it->first)) {
            it = nodeStates.erase(it);
        } else {
            ++it;
        }
    }
}

void CSigSharesManager::BanNode(NodeId nodeId)
{
    if (nodeId == -1) {
        return;
    }

    m_peerman.Misbehaving(nodeId, 100);

    LOCK(cs);
    auto it = nodeStates.find(nodeId);
    if (it == nodeStates.end()) {
        return;
    }

    auto& nodeState = it->second;
    nodeState.requestedSigShares.Clear();
    nodeState.banned = true;
}

void CSigSharesManager::WorkThreadMain()
{
    int64_t lastSendTime = 0;

    while (!workInterrupt) {
        RemoveBannedNodeStates();

        bool fMoreWork = ProcessPendingSigShares();
        SignPendingSigShares();

        if (TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()) - lastSendTime > 100) {
            SendMessages();
            lastSendTime = TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now());
        }

        Cleanup();

        // TODO Wakeup when pending signing is needed?
        if (!fMoreWork && !workInterrupt.sleep_for(std::chrono::milliseconds(100))) {
            return;
        }
    }
}

void CSigSharesManager::AsyncSign(CQuorumCPtr quorum, const uint256& id, const uint256& msgHash)
{
    LOCK(cs_pendingSigns);
    pendingSigns.emplace_back(std::move(quorum), id, msgHash);
}

void CSigSharesManager::SignPendingSigShares()
{
    std::vector<PendingSignatureData> v;
    WITH_LOCK(cs_pendingSigns, v.swap(pendingSigns));

    for (const auto& [pQuorum, id, msgHash] : v) {
        auto opt_sigShare = CreateSigShare(*pQuorum, id, msgHash);

        if (opt_sigShare.has_value() && opt_sigShare->sigShare.Get().IsValid()) {
            auto sigShare = *opt_sigShare;
            ProcessSigShare(sigShare, pQuorum);

            if (IsAllMembersConnectedEnabled(pQuorum->params.type, m_sporkman)) {
                LOCK(cs);
                auto& session = signedSessions[sigShare.GetSignHash()];
                session.sigShare = sigShare;
                session.quorum = pQuorum;
                session.nextAttemptTime = 0;
                session.attempt = 0;
            }
        }
    }
}

std::optional<CSigShare> CSigSharesManager::CreateSigShare(const CQuorum& quorum, const uint256& id, const uint256& msgHash) const
{
    cxxtimer::Timer t(true);
    auto activeMasterNodeProTxHash = m_mn_activeman.GetProTxHash();

    if (!quorum.IsValidMember(activeMasterNodeProTxHash)) {
        return std::nullopt;
    }

    if (quorum.params.is_single_member()) {
        int memberIdx = quorum.GetMemberIndex(activeMasterNodeProTxHash);
        if (memberIdx == -1) {
            // this should really not happen (IsValidMember gave true)
            return std::nullopt;
        }

        CSigShare sigShare(quorum.params.type, quorum.qc->quorumHash, id, msgHash, uint16_t(memberIdx), {});
        uint256 signHash = sigShare.buildSignHash().Get();

        // TODO: This one should be SIGN by QUORUM key, not by OPERATOR key
        // see TODO in CDKGSession::FinalizeSingleCommitment for details
        sigShare.sigShare.Set(m_mn_activeman.Sign(signHash, bls::bls_legacy_scheme.load()), bls::bls_legacy_scheme.load());

        if (!sigShare.sigShare.Get().IsValid()) {
            LogPrintf("CSigSharesManager::%s -- failed to sign sigShare. signHash=%s, id=%s, msgHash=%s, time=%s\n",
                      __func__, signHash.ToString(), sigShare.getId().ToString(), sigShare.getMsgHash().ToString(),
                      t.count());
            return std::nullopt;
        }

        sigShare.UpdateKey();

        LogPrint(BCLog::LLMQ_SIGS, /* Continued */
                 "CSigSharesManager::%s -- created sigShare. signHash=%s, id=%s, msgHash=%s, llmqType=%d, quorum=%s, "
                 "time=%s\n",
                 __func__, signHash.ToString(), sigShare.getId().ToString(), sigShare.getMsgHash().ToString(),
                 ToUnderlying(quorum.params.type), quorum.qc->quorumHash.ToString(), t.count());

        return sigShare;
    }
    const CBLSSecretKey& skShare = quorum.GetSkShare();
    if (!skShare.IsValid()) {
        LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- we don't have our skShare for quorum %s\n", __func__, quorum.qc->quorumHash.ToString());
        return std::nullopt;
    }

    int memberIdx = quorum.GetMemberIndex(activeMasterNodeProTxHash);
    if (memberIdx == -1) {
        // this should really not happen (IsValidMember gave true)
        return std::nullopt;
    }

    CSigShare sigShare(quorum.params.type, quorum.qc->quorumHash, id, msgHash, uint16_t(memberIdx), {});
    uint256 signHash = sigShare.buildSignHash().Get();

    sigShare.sigShare.Set(skShare.Sign(signHash, bls::bls_legacy_scheme.load()), bls::bls_legacy_scheme.load());
    if (!sigShare.sigShare.Get().IsValid()) {
        LogPrintf("CSigSharesManager::%s -- failed to sign sigShare. signHash=%s, id=%s, msgHash=%s, time=%s\n", __func__,
                  signHash.ToString(), sigShare.getId().ToString(), sigShare.getMsgHash().ToString(), t.count());
        return std::nullopt;
    }

    sigShare.UpdateKey();

    LogPrint(BCLog::LLMQ_SIGS, "CSigSharesManager::%s -- created sigShare. signHash=%s, id=%s, msgHash=%s, llmqType=%d, quorum=%s, time=%s\n", __func__,
              signHash.ToString(), sigShare.getId().ToString(), sigShare.getMsgHash().ToString(), ToUnderlying(quorum.params.type), quorum.qc->quorumHash.ToString(), t.count());

    return sigShare;
}

MessageProcessingResult CSigSharesManager::HandleNewRecoveredSig(const llmq::CRecoveredSig& recoveredSig)
{
    auto signHash = recoveredSig.buildSignHash().Get();
    LOCK(cs);
    RemoveSigSharesForSession(signHash);
    return {};
}
} // namespace llmq
