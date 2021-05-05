// Copyright (c) 2019-2021 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/quorums_chainlocks.h>
#include <llmq/quorums.h>
#include <llmq/quorums_commitment.h>
#include <llmq/quorums_instantsend.h>
#include <llmq/quorums_utils.h>

#include <chain.h>
#include <consensus/validation.h>
#include <masternode/activemasternode.h>
#include <masternode/masternode-sync.h>
#include <net_processing.h>
#include <scheduler.h>
#include <spork.h>
#include <txmempool.h>

namespace llmq
{

const std::string CLSIG_REQUESTID_PREFIX = "clsig";

CChainLocksHandler* chainLocksHandler;

bool CChainLockSig::IsNull() const
{
    return nHeight == -1 && blockHash == uint256();
}

std::string CChainLockSig::ToString() const
{
    return strprintf("CChainLockSig(nVersion=%d, nHeight=%d, blockHash=%s, signers: hex=%s size=%d count=%d)",
                nVersion, nHeight, blockHash.ToString(), CLLMQUtils::ToHexStr(signers), signers.size(),
                std::count(signers.begin(), signers.end(), true));
}

CChainLocksHandler::CChainLocksHandler()
{
    scheduler = new CScheduler();
    CScheduler::Function serviceLoop = boost::bind(&CScheduler::serviceQueue, scheduler);
    scheduler_thread = new boost::thread(boost::bind(&TraceThread<CScheduler::Function>, "cl-schdlr", serviceLoop));
}

CChainLocksHandler::~CChainLocksHandler()
{
    scheduler_thread->interrupt();
    scheduler_thread->join();
    delete scheduler_thread;
    delete scheduler;
}

void CChainLocksHandler::Start()
{
    quorumSigningManager->RegisterRecoveredSigsListener(this);
    scheduler->scheduleEvery([&]() {
        CheckActiveState();
        EnforceBestChainLock();
        // regularly retry signing the current chaintip as it might have failed before due to missing islocks
        TrySignChainTip();
    }, 5000);
}

void CChainLocksHandler::Stop()
{
    scheduler->stop();
    quorumSigningManager->UnregisterRecoveredSigsListener(this);
}

bool CChainLocksHandler::AlreadyHave(const CInv& inv)
{
    LOCK(cs);
    return seenChainLocks.count(inv.hash) != 0;
}

bool CChainLocksHandler::GetChainLockByHash(const uint256& hash, llmq::CChainLockSig& ret)
{
    LOCK(cs);

    if (::SerializeHash(mostRecentChainLockShare) == hash) {
        ret = mostRecentChainLockShare;
        return true;
    }

    if (::SerializeHash(bestChainLockWithKnownBlock) == hash) {
        ret = bestChainLockWithKnownBlock;
        return true;
    }

    for (const auto& pair : bestChainLockCandidates) {
        if (::SerializeHash(*pair.second) == hash) {
            ret = *pair.second;
            return true;
        }
    }

    for (const auto& pair : bestChainLockShares) {
        for (const auto& pair2 : pair.second) {
            if (::SerializeHash(*pair2.second) == hash) {
                ret = *pair2.second;
                return true;
            }
        }
    }

    return false;
}

const CChainLockSig CChainLocksHandler::GetMostRecentChainLock()
{
    LOCK(cs);
    return mostRecentChainLockShare;
}

const CChainLockSig CChainLocksHandler::GetBestChainLock()
{
    LOCK(cs);
    return bestChainLockWithKnownBlock;
}

const std::map<CQuorumCPtr, CChainLockSigCPtr> CChainLocksHandler::GetBestChainLockShares()
{
    if (!AreMultiQuorumChainLocksEnabled()) {
        return {};
    }

    LOCK(cs);
    auto it = bestChainLockShares.find(bestChainLockWithKnownBlock.nHeight);
    if (it == bestChainLockShares.end()) {
        return {};
    }

    return it->second;
}

bool CChainLocksHandler::TryUpdateBestChainLock(const CBlockIndex* pindex)
{
    AssertLockHeld(cs);

    if (pindex == nullptr || pindex->nHeight <= bestChainLockWithKnownBlock.nHeight) {
        return false;
    }

    auto it1 = bestChainLockCandidates.find(pindex->nHeight);
    if (it1 != bestChainLockCandidates.end()) {
        bestChainLockWithKnownBlock = *it1->second;
        bestChainLockBlockIndex = pindex;
        LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- CLSIG from candidates (%s)\n", __func__, bestChainLockWithKnownBlock.ToString());
        return true;
    }

    auto it2 = bestChainLockShares.find(pindex->nHeight);
    if (it2 == bestChainLockShares.end()) {
        return false;
    }

    const auto llmqType = Params().GetConsensus().llmqTypeChainLocks;
    const size_t threshold = GetLLMQParams(llmqType).signingActiveQuorumCount / 2 + 1;

    std::vector<CBLSSignature> sigs;
    CChainLockSig clsigAgg(1);

    for (const auto& pair : it2->second) {
        if (pair.second->blockHash == pindex->GetBlockHash()) {
            assert(std::count(pair.second->signers.begin(), pair.second->signers.end(), true) <= 1);
            sigs.emplace_back(pair.second->sig);
            if (clsigAgg.IsNull()) {
                clsigAgg = *pair.second;
            } else {
                assert(clsigAgg.signers.size() == pair.second->signers.size());
                std::transform(clsigAgg.signers.begin(), clsigAgg.signers.end(), pair.second->signers.begin(), clsigAgg.signers.begin(), std::logical_or<bool>());
            }
            if (sigs.size() >= threshold) {
                // all sigs should be validated already
                clsigAgg.sig = CBLSSignature::AggregateInsecure(sigs);
                bestChainLockWithKnownBlock = clsigAgg;
                bestChainLockBlockIndex = pindex;
                bestChainLockCandidates[clsigAgg.nHeight] = std::make_shared<const CChainLockSig>(clsigAgg);
                LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- CLSIG aggregated (%s)\n", __func__, bestChainLockWithKnownBlock.ToString());
                return true;
            }
        }
    }
    return false;
}

bool CChainLocksHandler::VerifyChainLockShare(const CChainLockSig& clsig, const CBlockIndex* pindexScan, const uint256& idIn, std::pair<int, CQuorumCPtr>& ret)
{
    AssertLockNotHeld(cs);

    const auto llmqType = Params().GetConsensus().llmqTypeChainLocks;
    const auto signingActiveQuorumCount = GetLLMQParams(llmqType).signingActiveQuorumCount;

    if (!AreMultiQuorumChainLocksEnabled()) {
        return false;
    }

    if (clsig.signers.size() != signingActiveQuorumCount) {
        return false;
    }

    if (std::count(clsig.signers.begin(), clsig.signers.end(), true) > 1) {
        // too may signers
        return false;
    }
    bool fHaveSigner{std::count(clsig.signers.begin(), clsig.signers.end(), true) > 0};

    const auto quorums_scanned = llmq::quorumManager->ScanQuorums(llmqType, pindexScan, signingActiveQuorumCount);

    for (size_t i = 0; i < quorums_scanned.size(); ++i) {
        const CQuorumCPtr& quorum = quorums_scanned[i];
        if (quorum == nullptr) {
            return false;
        }
        uint256 requestId = ::SerializeHash(std::make_tuple(CLSIG_REQUESTID_PREFIX, clsig.nHeight, quorum->qc->quorumHash));
        if ((!idIn.IsNull() && idIn != requestId)) {
            continue;
        }
        if (fHaveSigner && !clsig.signers[i]) {
            continue;
        }
        uint256 signHash = CLLMQUtils::BuildSignHash(llmqType, quorum->qc->quorumHash, requestId, clsig.blockHash);
        LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- CLSIG (%s) requestId=%s, signHash=%s\n",
                __func__, clsig.ToString(), requestId.ToString(), signHash.ToString());

        if (clsig.sig.VerifyInsecure(quorum->qc->quorumPublicKey, signHash)) {
            if (idIn.IsNull() && !quorumSigningManager->HasRecoveredSigForId(llmqType, requestId)) {
                // We can reconstruct the CRecoveredSig from the clsig and pass it to the signing manager, which
                // avoids unnecessary double-verification of the signature. We can do this here because we just
                // verified the sig.
                std::shared_ptr<CRecoveredSig> rs = std::make_shared<CRecoveredSig>();
                rs->llmqType = llmqType;
                rs->quorumHash = quorum->qc->quorumHash;
                rs->id = requestId;
                rs->msgHash = clsig.blockHash;
                rs->sig.Set(clsig.sig);
                rs->UpdateHash();
                quorumSigningManager->PushReconstructedRecoveredSig(rs);
            }
            ret = std::make_pair(i, quorum);
            return true;
        }
        if (!idIn.IsNull() || fHaveSigner) {
            return false;
        }
    }
    return false;
}

bool CChainLocksHandler::VerifyAggregatedChainLock(const CChainLockSig& clsig, const CBlockIndex* pindexScan)
{
    AssertLockNotHeld(cs);

    const auto llmqType = Params().GetConsensus().llmqTypeChainLocks;
    const auto signingActiveQuorumCount = GetLLMQParams(llmqType).signingActiveQuorumCount;

    std::vector<uint256> hashes;
    std::vector<CBLSPublicKey> quorumPublicKeys;

    if (!AreMultiQuorumChainLocksEnabled()) {
        return false;
    }

    if (clsig.signers.size() != signingActiveQuorumCount) {
        return false;
    }

    if (std::count(clsig.signers.begin(), clsig.signers.end(), true) < (signingActiveQuorumCount / 2 + 1)) {
        // not enough signers
        return false;
    }

    const auto quorums_scanned = llmq::quorumManager->ScanQuorums(llmqType, pindexScan, signingActiveQuorumCount);

    for (size_t i = 0; i < quorums_scanned.size(); ++i) {
        const CQuorumCPtr& quorum = quorums_scanned[i];
        if (quorum == nullptr) {
            return false;
        }
        if (!clsig.signers[i]) {
            continue;
        }
        quorumPublicKeys.emplace_back(quorum->qc->quorumPublicKey);
        uint256 requestId = ::SerializeHash(std::make_tuple(CLSIG_REQUESTID_PREFIX, clsig.nHeight, quorum->qc->quorumHash));
        uint256 signHash = CLLMQUtils::BuildSignHash(llmqType, quorum->qc->quorumHash, requestId, clsig.blockHash);
        hashes.emplace_back(signHash);
        LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- CLSIG (%s) requestId=%s, signHash=%s\n",
                __func__, clsig.ToString(), requestId.ToString(), signHash.ToString());
    }
    return clsig.sig.VerifyInsecureAggregated(quorumPublicKeys, hashes);
}

void CChainLocksHandler::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv)
{
    if (!AreChainLocksEnabled()) {
        return;
    }

    if (strCommand == NetMsgType::CLSIG || strCommand == NetMsgType::CLSIGMQ) {
        CChainLockSig clsig(strCommand == NetMsgType::CLSIGMQ ? 1 : 0);
        vRecv >> clsig;

        auto hash = ::SerializeHash(clsig);

        ProcessNewChainLock(pfrom->GetId(), clsig, hash);
    }
}

void CChainLocksHandler::ProcessNewChainLock(const NodeId from, CChainLockSig& clsig, const uint256& hash, const uint256& idIn)
{
    assert((from == -1) ^ idIn.IsNull());

    CheckActiveState();

    CInv clsigInv(clsig.nVersion == 1 ? MSG_CLSIGMQ : MSG_CLSIG, hash);

    if (from != -1) {
        LOCK(cs_main);
        EraseObjectRequest(from, clsigInv);
    }

    {
        LOCK(cs);
        if (!seenChainLocks.emplace(hash, GetTimeMillis()).second) {
            return;
        }

        if (!bestChainLockWithKnownBlock.IsNull() && clsig.nHeight <= bestChainLockWithKnownBlock.nHeight) {
            // no need to process/relay older CLSIGs
            return;
        }
    }

    CBlockIndex* pindexSig;
    CBlockIndex* pindexScan;
    {
        LOCK(cs_main);
        if (clsig.nHeight > chainActive.Height() + CSigningManager::SIGN_HEIGHT_OFFSET) {
            // too far into the future
            LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- future CLSIG (%s), peer=%d\n", __func__, clsig.ToString(), from);
            return;
        }
        pindexSig = pindexScan = LookupBlockIndex(clsig.blockHash);
        if (pindexScan == nullptr) {
            // we don't know the block/header for this CLSIG yet
            if (clsig.nHeight <= chainActive.Height()) {
                // could be a parallel fork at the same height, try scanning quorums at the same height
                pindexScan = chainActive.Tip()->GetAncestor(clsig.nHeight);
            } else {
                // no idea what kind of block it is, try scanning quorums at chain tip
                pindexScan = chainActive.Tip();
            }
        }
        if (pindexSig != nullptr && pindexSig->nHeight != clsig.nHeight) {
            // Should not happen
            LogPrintf("CChainLocksHandler::%s -- height of CLSIG (%s) does not match the expected block's height (%d)\n",
                    __func__, clsig.ToString(), pindexSig->nHeight);
            return;
        }
    }

    const auto llmqType = Params().GetConsensus().llmqTypeChainLocks;
    const auto signingActiveQuorumCount = GetLLMQParams(llmqType).signingActiveQuorumCount;

    if (AreMultiQuorumChainLocksEnabled()) {
        size_t signers_count = std::count(clsig.signers.begin(), clsig.signers.end(), true);
        if (from != -1 && (clsig.signers.empty() || signers_count == 0)) {
            LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- invalid signers count (%d) for CLSIG (%s), peer=%d\n", __func__, signers_count, clsig.ToString(), from);
            LOCK(cs_main);
            Misbehaving(from, 10);
            return;
        }
        if (from == -1 || signers_count == 1) {
            // A part of a multi-quorum CLSIG signed by a single quorum
            std::pair<int, CQuorumCPtr> ret;
            clsig.signers.resize(signingActiveQuorumCount, false);
            if (!VerifyChainLockShare(clsig, pindexScan, idIn, ret)) {
                LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- invalid CLSIG (%s), peer=%d\n", __func__, clsig.ToString(), from);
                if (from != -1) {
                    LOCK(cs_main);
                    Misbehaving(from, 10);
                }
                return;
            }
            CInv clsigAggInv;
            {
                LOCK(cs);
                clsig.signers[ret.first] = true;
                if (std::count(clsig.signers.begin(), clsig.signers.end(), true) > 1) {
                    // this shouldn never happen
                    LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- ERROR in VerifyChainLockShare, CLSIG (%s), peer=%d\n", __func__, clsig.ToString(), from);
                    return;
                }
                auto it = bestChainLockShares.find(clsig.nHeight);
                if (it == bestChainLockShares.end()) {
                    bestChainLockShares[clsig.nHeight].emplace(ret.second, std::make_shared<const CChainLockSig>(clsig));
                } else {
                    it->second.emplace(ret.second, std::make_shared<const CChainLockSig>(clsig));
                }
                mostRecentChainLockShare = clsig;
                if (TryUpdateBestChainLock(pindexSig)) {
                    clsigAggInv = CInv(MSG_CLSIGMQ, ::SerializeHash(bestChainLockWithKnownBlock));
                }
            }
            // Note: do not hold cs while calling RelayInv
            AssertLockNotHeld(cs);
            if (clsigAggInv.type == MSG_CLSIGMQ) {
                // We just created an aggregated CLSIG, relay it
                g_connman->RelayInv(clsigAggInv, MULTI_QUORUM_CHAINLOCKS_VERSION);
            } else {
                // Relay partial CLSIGs to full nodes only, SPV wallets should wait for the aggregated CLSIG.
                g_connman->ForEachNode([&](CNode* pnode) {
                    bool fSPV{false};
                    {
                        LOCK(pnode->cs_filter);
                        fSPV = pnode->pfilter != nullptr;
                    }
                    if (pnode->nVersion >= MULTI_QUORUM_CHAINLOCKS_VERSION && !fSPV && pnode->CanRelay()) {
                        pnode->PushInventory(clsigInv);
                    }
                });
                // Try signing the tip ourselves
                TrySignChainTip();
            }
        } else {
            // An aggregated CLSIG
            if (!VerifyAggregatedChainLock(clsig, pindexScan)) {
                LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- invalid CLSIG (%s), peer=%d\n", __func__, clsig.ToString(), from);
                if (from != -1) {
                    LOCK(cs_main);
                    Misbehaving(from, 10);
                }
                return;
            }
            {
                LOCK(cs);
                bestChainLockCandidates[clsig.nHeight] = std::make_shared<const CChainLockSig>(clsig);
                mostRecentChainLockShare = clsig;
                TryUpdateBestChainLock(pindexSig);
            }
            // Note: do not hold cs while calling RelayInv
            AssertLockNotHeld(cs);
            g_connman->RelayInv(clsigInv, MULTI_QUORUM_CHAINLOCKS_VERSION);
        }
    } else {
        if (!clsig.signers.empty()) {
            LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- non-empty signers for CLSIG (%s), peer=%d\n", __func__, clsig.ToString(), from);
            if (from != -1) {
                LOCK(cs_main);
                Misbehaving(from, 10);
            }
            return;
        }
        uint256 requestId = ::SerializeHash(std::make_pair(CLSIG_REQUESTID_PREFIX, clsig.nHeight));
        if (!idIn.IsNull() && idIn != requestId) {
            // this should never happen
            LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- invalid CLSIG (%s), peer=%d\n", __func__, clsig.ToString(), from);
            return;
        }
        auto quorum = CSigningManager::SelectQuorumForSigning(llmqType, requestId, clsig.nHeight);
        if (quorum == nullptr) {
            return;
        }
        uint256 signHash = CLLMQUtils::BuildSignHash(llmqType, quorum->qc->quorumHash, requestId, clsig.blockHash);
        LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- CLSIG (%s) requestId=%s, signHash=%s, peer=%d\n",
                __func__, clsig.ToString(), requestId.ToString(), signHash.ToString(), from);

        if (!clsig.sig.VerifyInsecure(quorum->qc->quorumPublicKey, signHash)) {
            LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- invalid CLSIG (%s), peer=%d\n", __func__, clsig.ToString(), from);
            if (from != -1) {
                LOCK(cs_main);
                Misbehaving(from, 10);
            }
            return;
        }

        if (idIn.IsNull() && !quorumSigningManager->HasRecoveredSigForId(llmqType, requestId)) {
            // We can reconstruct the CRecoveredSig from the clsig and pass it to the signing manager, which
            // avoids unnecessary double-verification of the signature. We can do this here because we just
            // verified the sig.
            std::shared_ptr<CRecoveredSig> rs = std::make_shared<CRecoveredSig>();
            rs->llmqType = llmqType;
            rs->quorumHash = quorum->qc->quorumHash;
            rs->id = requestId;
            rs->msgHash = clsig.blockHash;
            rs->sig.Set(clsig.sig);
            rs->UpdateHash();
            quorumSigningManager->PushReconstructedRecoveredSig(rs);
        }

        {
            LOCK(cs);
            bestChainLockCandidates[clsig.nHeight] = std::make_shared<const CChainLockSig>(clsig);
            mostRecentChainLockShare = clsig;
            TryUpdateBestChainLock(pindexSig);
        }
        // Note: do not hold cs while calling RelayInv
        AssertLockNotHeld(cs);
        g_connman->RelayInv(clsigInv, LLMQS_PROTO_VERSION);
    }

    if (pindexSig == nullptr) {
        // we don't know the block/header for this CLSIG yet, so bail out for now
        // when the block or the header later comes in, we will enforce the correct chain
        return;
    }

    if (bestChainLockBlockIndex == pindexSig) {
        scheduler->scheduleFromNow([&]() {
            CheckActiveState();
            EnforceBestChainLock();
        }, 0);
    }

    LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- processed new CLSIG (%s), peer=%d\n",
              __func__, clsig.ToString(), from);
}

void CChainLocksHandler::AcceptedBlockHeader(const CBlockIndex* pindexNew)
{
    LOCK(cs);

    auto it = bestChainLockCandidates.find(pindexNew->nHeight);
    if (it == bestChainLockCandidates.end()) {
        return;
    }

    LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- block header %s came in late, updating and enforcing\n", __func__, pindexNew->GetBlockHash().ToString());

    // when EnforceBestChainLock is called later, it might end up invalidating other chains but not activating the
    // CLSIG locked chain. This happens when only the header is known but the block is still missing yet. The usual
    // block processing logic will handle this when the block arrives
    TryUpdateBestChainLock(pindexNew);
}

void CChainLocksHandler::UpdatedBlockTip(const CBlockIndex* pindexNew)
{
    // don't call TrySignChainTip directly but instead let the scheduler call it. This way we ensure that cs_main is
    // never locked and TrySignChainTip is not called twice in parallel. Also avoids recursive calls due to
    // EnforceBestChainLock switching chains.
    LOCK(cs);
    if (tryLockChainTipScheduled) {
        return;
    }
    tryLockChainTipScheduled = true;
    scheduler->scheduleFromNow([&]() {
        CheckActiveState();
        EnforceBestChainLock();
        TrySignChainTip();
        LOCK(cs);
        tryLockChainTipScheduled = false;
    }, 0);
}

void CChainLocksHandler::CheckActiveState()
{
    bool fDIP0008Active;
    {
        LOCK(cs_main);
        fDIP0008Active = chainActive.Tip() && chainActive.Tip()->pprev && chainActive.Tip()->pprev->nHeight >= Params().GetConsensus().DIP0008Height;
    }

    LOCK(cs);
    bool oldIsEnforced = isEnforced;
    isEnabled = AreChainLocksEnabled();
    isEnforced = (fDIP0008Active && isEnabled);

    if (!oldIsEnforced && isEnforced) {
        // ChainLocks got activated just recently, but it's possible that it was already running before, leaving
        // us with some stale values which we should not try to enforce anymore (there probably was a good reason
        // to disable spork19)
        mostRecentChainLockShare = bestChainLockWithKnownBlock = CChainLockSig();
        bestChainLockBlockIndex = lastNotifyChainLockBlockIndex = nullptr;
        bestChainLockCandidates.clear();
        bestChainLockShares.clear();
    }
}

void CChainLocksHandler::TrySignChainTip()
{
    const static int attempt_start{-2}; // let a couple of extra attempts to wait for busy/slow quorums
    static int attempt{attempt_start};
    static int lastSignedHeight{-1};

    Cleanup();

    if (!fMasternodeMode) {
        return;
    }

    if (!masternodeSync.IsBlockchainSynced()) {
        return;
    }

    const CBlockIndex* pindex;
    {
        LOCK(cs_main);
        pindex = chainActive.Tip();
    }

    if (!pindex->pprev) {
        return;
    }

    {
        LOCK(cs);

        if (!isEnabled) {
            return;
        }

        if (pindex->nHeight == lastSignedHeight) {
            // already signed this one
            return;
        }

        if (bestChainLockWithKnownBlock.nHeight >= pindex->nHeight) {
            // already got the same CLSIG or a better one, reset and bail out
            lastSignedHeight = bestChainLockWithKnownBlock.nHeight;
            attempt = attempt_start;
            return;
        }

        if (InternalHasConflictingChainLock(pindex->nHeight, pindex->GetBlockHash())) {
            // don't sign if another conflicting CLSIG is already present. EnforceBestChainLock will later enforce
            // the correct chain.
            return;
        }
    }

    LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- trying to sign %s, height=%d\n", __func__, pindex->GetBlockHash().ToString(), pindex->nHeight);

    // When the new IX system is activated, we only try to ChainLock blocks which include safe transactions. A TX is
    // considered safe when it is islocked or at least known since 10 minutes (from mempool or block). These checks are
    // performed for the tip (which we try to sign) and the previous 5 blocks. If a ChainLocked block is found on the
    // way down, we consider all TXs to be safe.
    if (IsInstantSendEnabled() && RejectConflictingBlocks()) {
        auto pindexWalk = pindex;
        while (pindexWalk) {
            if (pindex->nHeight - pindexWalk->nHeight > 5) {
                // no need to check further down, 6 confs is safe to assume that TXs below this height won't be
                // islocked anymore if they aren't already
                LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- tip and previous 5 blocks all safe\n", __func__);
                break;
            }
            if (HasChainLock(pindexWalk->nHeight, pindexWalk->GetBlockHash())) {
                // we don't care about islocks for TXs that are ChainLocked already
                LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- chainlock at height %d\n", __func__, pindexWalk->nHeight);
                break;
            }

            auto txids = GetBlockTxs(pindexWalk->GetBlockHash());
            if (!txids) {
                pindexWalk = pindexWalk->pprev;
                continue;
            }

            for (auto& txid : *txids) {
                int64_t txAge = 0;
                {
                    LOCK(cs);
                    auto it = txFirstSeenTime.find(txid);
                    if (it != txFirstSeenTime.end()) {
                        txAge = GetAdjustedTime() - it->second;
                    }
                }

                if (txAge < WAIT_FOR_ISLOCK_TIMEOUT && !quorumInstantSendManager->IsLocked(txid)) {
                    LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- not signing block %s due to TX %s not being islocked and not old enough. age=%d\n", __func__,
                              pindexWalk->GetBlockHash().ToString(), txid.ToString(), txAge);
                    return;
                }
            }

            pindexWalk = pindexWalk->pprev;
        }
    }

    const auto llmqType = Params().GetConsensus().llmqTypeChainLocks;
    const auto signingActiveQuorumCount = GetLLMQParams(llmqType).signingActiveQuorumCount;
    mapSignedRequestIds.clear();

    if (AreMultiQuorumChainLocksEnabled()) {
        // Use multiple ChainLocks quorums
        const auto quorums_scanned = llmq::quorumManager->ScanQuorums(llmqType, pindex, signingActiveQuorumCount);
        std::map<CQuorumCPtr, CChainLockSigCPtr> mapSharesAtTip;
        {
            LOCK(cs);
            const auto it = bestChainLockShares.find(pindex->nHeight);
            if (it != bestChainLockShares.end()) {
                mapSharesAtTip = it->second;
            }
        }
        bool fMemberOfSomeQuorum{false};
        ++attempt;
        for (size_t i = 0; i < quorums_scanned.size(); ++i) {
            int nQuorumIndex = (pindex->nHeight + i) % quorums_scanned.size();
            const CQuorumCPtr& quorum = quorums_scanned[nQuorumIndex];
            if (quorum == nullptr) {
                return;
            }
            if (!quorum->IsValidMember(activeMasternodeInfo.proTxHash)) {
                continue;
            }
            fMemberOfSomeQuorum = true;
            if (i > 0) {
                int nQuorumIndexPrev = (nQuorumIndex + 1) % quorums_scanned.size();
                auto it2 = mapSharesAtTip.find(quorums_scanned[nQuorumIndexPrev]);
                if (it2 == mapSharesAtTip.end() && attempt > i) {
                    // look deeper after 'i' attempts
                    while (nQuorumIndexPrev != nQuorumIndex) {
                        nQuorumIndexPrev = (nQuorumIndexPrev + 1) % quorums_scanned.size();
                        it2 = mapSharesAtTip.find(quorums_scanned[nQuorumIndexPrev]);
                        if (it2 != mapSharesAtTip.end()) {
                            break;
                        }
                        LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- previous quorum (%d, %s) didn't sign a chainlock at height %d yet\n",
                                __func__, nQuorumIndexPrev, quorums_scanned[nQuorumIndexPrev]->qc->quorumHash.ToString(), pindex->nHeight);
                    }
                }
                if (it2 == mapSharesAtTip.end()) {
                    if (attempt <= i) {
                        // previous quorum did not sign a chainlock, bail out for now
                        LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- previous quorum did not sign a chainlock at height %d yet\n", __func__, pindex->nHeight);
                        return;
                    }
                    // else
                    // waiting for previous quorum(s) to sign anything for too long already,
                    // just sign whatever we think is a good tip
                } else if (it2->second->blockHash != pindex->GetBlockHash()) {
                    LOCK(cs_main);
                    auto shareBlockIndex = LookupBlockIndex(it2->second->blockHash);
                    if (shareBlockIndex != nullptr && shareBlockIndex->nHeight == pindex->nHeight) {
                        // previous quorum signed an alternative chain tip, sign it too instead
                        LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- previous quorum (%d, %s) signed an altenative chaintip (%s != %s) at height %d, join it\n",
                                __func__, nQuorumIndexPrev, quorums_scanned[nQuorumIndexPrev]->qc->quorumHash.ToString(), it2->second->blockHash.ToString(), pindex->GetBlockHash().ToString(), pindex->nHeight);
                        pindex = shareBlockIndex;
                    } else if (attempt <= i) {
                        // previous quorum signed some different hash we have no idea about, bail out for now
                        LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- previous quorum (%d, %s) signed an unknown or an invalid blockHash (%s != %s) at height %d\n",
                                __func__, nQuorumIndexPrev, quorums_scanned[nQuorumIndexPrev]->qc->quorumHash.ToString(), it2->second->blockHash.ToString(), pindex->GetBlockHash().ToString(), pindex->nHeight);
                        return;
                    }
                    // else
                    // waiting the unknown/invalid hash for too long already,
                    // just sign whatever we think is a good tip
                }
            }
            LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- use quorum (%d, %s) and try to sign %s at height %d\n",
                    __func__, nQuorumIndex, quorums_scanned[nQuorumIndex]->qc->quorumHash.ToString(), pindex->GetBlockHash().ToString(), pindex->nHeight);
            uint256 requestId = ::SerializeHash(std::make_tuple(CLSIG_REQUESTID_PREFIX, pindex->nHeight, quorum->qc->quorumHash));
            {
                LOCK(cs);
                if (bestChainLockWithKnownBlock.nHeight >= pindex->nHeight) {
                    // might have happened while we didn't hold cs
                    return;
                }
                mapSignedRequestIds.emplace(requestId, std::make_pair(pindex->nHeight, pindex->GetBlockHash()));
            }
            quorumSigningManager->AsyncSignIfMember(llmqType, requestId, pindex->GetBlockHash(), quorum->qc->quorumHash);
        }
        if (!fMemberOfSomeQuorum || attempt >= quorums_scanned.size()) {
            // not a member or tried too many times, nothing to do
            lastSignedHeight = pindex->nHeight;
            attempt = attempt_start;
        }
    } else {
        // Use single ChainLocks quorum
        uint256 requestId = ::SerializeHash(std::make_pair(CLSIG_REQUESTID_PREFIX, pindex->nHeight));
        {
            LOCK(cs);
            if (bestChainLockWithKnownBlock.nHeight >= pindex->nHeight) {
                // might have happened while we didn't hold cs
                return;
            }
            mapSignedRequestIds.emplace(requestId, std::make_pair(pindex->nHeight, pindex->GetBlockHash()));
        }
        quorumSigningManager->AsyncSignIfMember(llmqType, requestId, pindex->GetBlockHash());
        lastSignedHeight = pindex->nHeight;
    }
}

void CChainLocksHandler::TransactionAddedToMempool(const CTransactionRef& tx, int64_t nAcceptTime)
{
    if (tx->IsCoinBase() || tx->vin.empty()) {
        return;
    }

    LOCK(cs);
    txFirstSeenTime.emplace(tx->GetHash(), nAcceptTime);
}

void CChainLocksHandler::BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex, const std::vector<CTransactionRef>& vtxConflicted)
{
    if (!masternodeSync.IsBlockchainSynced()) {
        return;
    }

    // We listen for BlockConnected so that we can collect all TX ids of all included TXs of newly received blocks
    // We need this information later when we try to sign a new tip, so that we can determine if all included TXs are
    // safe.

    LOCK(cs);

    auto it = blockTxs.find(pindex->GetBlockHash());
    if (it == blockTxs.end()) {
        // we must create this entry even if there are no lockable transactions in the block, so that TrySignChainTip
        // later knows about this block
        it = blockTxs.emplace(pindex->GetBlockHash(), std::make_shared<std::unordered_set<uint256, StaticSaltedHasher>>()).first;
    }
    auto& txids = *it->second;

    int64_t curTime = GetAdjustedTime();

    for (const auto& tx : pblock->vtx) {
        if (tx->IsCoinBase() || tx->vin.empty()) {
            continue;
        }

        txids.emplace(tx->GetHash());
        txFirstSeenTime.emplace(tx->GetHash(), curTime);
    }

}

void CChainLocksHandler::BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexDisconnected)
{
    LOCK(cs);
    blockTxs.erase(pindexDisconnected->GetBlockHash());
}

CChainLocksHandler::BlockTxs::mapped_type CChainLocksHandler::GetBlockTxs(const uint256& blockHash)
{
    AssertLockNotHeld(cs);
    AssertLockNotHeld(cs_main);

    CChainLocksHandler::BlockTxs::mapped_type ret;

    {
        LOCK(cs);
        auto it = blockTxs.find(blockHash);
        if (it != blockTxs.end()) {
            ret = it->second;
        }
    }
    if (!ret) {
        // This should only happen when freshly started.
        // If running for some time, SyncTransaction should have been called before which fills blockTxs.
        LogPrint(BCLog::CHAINLOCKS, "CChainLocksHandler::%s -- blockTxs for %s not found. Trying ReadBlockFromDisk\n", __func__,
                 blockHash.ToString());

        uint32_t blockTime;
        {
            LOCK(cs_main);
            auto pindex = LookupBlockIndex(blockHash);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
                return nullptr;
            }

            ret = std::make_shared<std::unordered_set<uint256, StaticSaltedHasher>>();
            for (auto& tx : block.vtx) {
                if (tx->IsCoinBase() || tx->vin.empty()) {
                    continue;
                }
                ret->emplace(tx->GetHash());
            }

            blockTime = block.nTime;
        }

        LOCK(cs);
        blockTxs.emplace(blockHash, ret);
        for (auto& txid : *ret) {
            txFirstSeenTime.emplace(txid, blockTime);
        }
    }
    return ret;
}

bool CChainLocksHandler::IsTxSafeForMining(const uint256& txid)
{
    if (!RejectConflictingBlocks()) {
        return true;
    }
    if (!IsInstantSendEnabled()) {
        return true;
    }

    int64_t txAge = 0;
    {
        LOCK(cs);
        if (!isEnabled || !isEnforced) {
            return true;
        }
        auto it = txFirstSeenTime.find(txid);
        if (it != txFirstSeenTime.end()) {
            txAge = GetAdjustedTime() - it->second;
        }
    }

    if (txAge < WAIT_FOR_ISLOCK_TIMEOUT && !quorumInstantSendManager->IsLocked(txid)) {
        return false;
    }
    return true;
}

// WARNING: cs_main and cs should not be held!
// This should also not be called from validation signals, as this might result in recursive calls
void CChainLocksHandler::EnforceBestChainLock()
{
    AssertLockNotHeld(cs);
    AssertLockNotHeld(cs_main);

    std::shared_ptr<CChainLockSig> clsig;
    const CBlockIndex* pindex;
    const CBlockIndex* currentBestChainLockBlockIndex;
    {
        LOCK(cs);

        if (!isEnforced) {
            return;
        }

        clsig = std::make_shared<CChainLockSig>(bestChainLockWithKnownBlock);
        pindex = currentBestChainLockBlockIndex = this->bestChainLockBlockIndex;

        if (!currentBestChainLockBlockIndex) {
            // we don't have the header/block, so we can't do anything right now
            return;
        }
    }

    bool activateNeeded;
    CValidationState state;
    const auto &params = Params();
    {
        LOCK(cs_main);

        // Go backwards through the chain referenced by clsig until we find a block that is part of the main chain.
        // For each of these blocks, check if there are children that are NOT part of the chain referenced by clsig
        // and mark all of them as conflicting.
        while (pindex && !chainActive.Contains(pindex)) {
            // Mark all blocks that have the same prevBlockHash but are not equal to blockHash as conflicting
            auto itp = mapPrevBlockIndex.equal_range(pindex->pprev->GetBlockHash());
            for (auto jt = itp.first; jt != itp.second; ++jt) {
                if (jt->second == pindex) {
                    continue;
                }
                if (!MarkConflictingBlock(state, params, jt->second)) {
                    LogPrintf("CChainLocksHandler::%s -- MarkConflictingBlock failed: %s\n", __func__, FormatStateMessage(state));
                    // This should not have happened and we are in a state were it's not safe to continue anymore
                    assert(false);
                }
                LogPrintf("CChainLocksHandler::%s -- CLSIG (%s) marked block %s as conflicting\n",
                          __func__, clsig->ToString(), jt->second->GetBlockHash().ToString());
            }

            pindex = pindex->pprev;
        }
        // In case blocks from the correct chain are invalid at the moment, reconsider them. The only case where this
        // can happen right now is when missing superblock triggers caused the main chain to be dismissed first. When
        // the trigger later appears, this should bring us to the correct chain eventually. Please note that this does
        // NOT enforce invalid blocks in any way, it just causes re-validation.
        if (!currentBestChainLockBlockIndex->IsValid()) {
            ResetBlockFailureFlags(LookupBlockIndex(currentBestChainLockBlockIndex->GetBlockHash()));
        }

        activateNeeded = chainActive.Tip()->GetAncestor(currentBestChainLockBlockIndex->nHeight) != currentBestChainLockBlockIndex;
    }

    if (activateNeeded && !ActivateBestChain(state, params)) {
        LogPrintf("CChainLocksHandler::%s -- ActivateBestChain failed: %s\n", __func__, FormatStateMessage(state));
    }

    const CBlockIndex* pindexNotify = nullptr;
    {
        LOCK(cs_main);
        if (lastNotifyChainLockBlockIndex != currentBestChainLockBlockIndex &&
            chainActive.Tip()->GetAncestor(currentBestChainLockBlockIndex->nHeight) == currentBestChainLockBlockIndex) {
            lastNotifyChainLockBlockIndex = currentBestChainLockBlockIndex;
            pindexNotify = currentBestChainLockBlockIndex;
        }
    }

    if (pindexNotify) {
        GetMainSignals().NotifyChainLock(pindexNotify, clsig);
    }
}

void CChainLocksHandler::HandleNewRecoveredSig(const llmq::CRecoveredSig& recoveredSig)
{
    CChainLockSig clsig(AreMultiQuorumChainLocksEnabled() ? 1 : 0);
    {
        LOCK(cs);

        if (!isEnabled) {
            return;
        }

        auto it = mapSignedRequestIds.find(recoveredSig.id);
        if (it == mapSignedRequestIds.end() || recoveredSig.msgHash != it->second.second) {
            // this is not what we signed, so lets not create a CLSIG for it
            return;
        }
        if (bestChainLockWithKnownBlock.nHeight >= it->second.first) {
            // already got the same or a better CLSIG through the CLSIG message
            return;
        }

        clsig.nHeight = it->second.first;
        clsig.blockHash = it->second.second;
        clsig.sig = recoveredSig.sig.Get();
        mapSignedRequestIds.erase(recoveredSig.id);
    }
    ProcessNewChainLock(-1, clsig, ::SerializeHash(clsig), recoveredSig.id);
}

bool CChainLocksHandler::HasChainLock(int nHeight, const uint256& blockHash)
{
    LOCK(cs);
    return InternalHasChainLock(nHeight, blockHash);
}

bool CChainLocksHandler::InternalHasChainLock(int nHeight, const uint256& blockHash)
{
    AssertLockHeld(cs);

    if (!isEnforced) {
        return false;
    }

    if (!bestChainLockBlockIndex) {
        return false;
    }

    if (nHeight > bestChainLockBlockIndex->nHeight) {
        return false;
    }

    if (nHeight == bestChainLockBlockIndex->nHeight) {
        return blockHash == bestChainLockBlockIndex->GetBlockHash();
    }

    auto pAncestor = bestChainLockBlockIndex->GetAncestor(nHeight);
    return pAncestor && pAncestor->GetBlockHash() == blockHash;
}

bool CChainLocksHandler::HasConflictingChainLock(int nHeight, const uint256& blockHash)
{
    LOCK(cs);
    return InternalHasConflictingChainLock(nHeight, blockHash);
}

bool CChainLocksHandler::InternalHasConflictingChainLock(int nHeight, const uint256& blockHash)
{
    AssertLockHeld(cs);

    if (!isEnforced) {
        return false;
    }

    if (!bestChainLockBlockIndex) {
        return false;
    }

    if (nHeight > bestChainLockBlockIndex->nHeight) {
        return false;
    }

    if (nHeight == bestChainLockBlockIndex->nHeight) {
        return blockHash != bestChainLockBlockIndex->GetBlockHash();
    }

    auto pAncestor = bestChainLockBlockIndex->GetAncestor(nHeight);
    assert(pAncestor);
    return pAncestor->GetBlockHash() != blockHash;
}

void CChainLocksHandler::Cleanup()
{
    if (!masternodeSync.IsBlockchainSynced()) {
        return;
    }

    {
        LOCK(cs);
        if (GetTimeMillis() - lastCleanupTime < CLEANUP_INTERVAL) {
            return;
        }
    }

    // need mempool.cs due to GetTransaction calls
    LOCK2(cs_main, mempool.cs);
    LOCK(cs);

    for (auto it = seenChainLocks.begin(); it != seenChainLocks.end(); ) {
        if (GetTimeMillis() - it->second >= CLEANUP_SEEN_TIMEOUT) {
            it = seenChainLocks.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = blockTxs.begin(); it != blockTxs.end(); ) {
        auto pindex = LookupBlockIndex(it->first);
        if (InternalHasChainLock(pindex->nHeight, pindex->GetBlockHash())) {
            for (auto& txid : *it->second) {
                txFirstSeenTime.erase(txid);
            }
            it = blockTxs.erase(it);
        } else if (InternalHasConflictingChainLock(pindex->nHeight, pindex->GetBlockHash())) {
            it = blockTxs.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = txFirstSeenTime.begin(); it != txFirstSeenTime.end(); ) {
        CTransactionRef tx;
        uint256 hashBlock;
        if (!GetTransaction(it->first, tx, Params().GetConsensus(), hashBlock)) {
            // tx has vanished, probably due to conflicts
            it = txFirstSeenTime.erase(it);
        } else if (!hashBlock.IsNull()) {
            auto pindex = LookupBlockIndex(hashBlock);
            if (chainActive.Tip()->GetAncestor(pindex->nHeight) == pindex && chainActive.Height() - pindex->nHeight >= 6) {
                // tx got confirmed >= 6 times, so we can stop keeping track of it
                it = txFirstSeenTime.erase(it);
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }

    if (bestChainLockBlockIndex != nullptr) {
        for (auto it = bestChainLockCandidates.begin(); it != bestChainLockCandidates.end(); ) {
            if (it->first == bestChainLockBlockIndex->nHeight) {
                it = bestChainLockCandidates.erase(++it, bestChainLockCandidates.end());
            } else {
                ++it;
            }
        }
        for (auto it = bestChainLockShares.begin(); it != bestChainLockShares.end(); ) {
            if (it->first == bestChainLockBlockIndex->nHeight) {
                it = bestChainLockShares.erase(++it, bestChainLockShares.end());
            } else {
                ++it;
            }
        }
    }

    lastCleanupTime = GetTimeMillis();
}

bool AreChainLocksEnabled()
{
    return sporkManager.IsSporkActive(SPORK_19_CHAINLOCKS_ENABLED);
}

bool AreMultiQuorumChainLocksEnabled()
{
    return sporkManager.GetSporkValue(SPORK_19_CHAINLOCKS_ENABLED) == 1;
}

} // namespace llmq
