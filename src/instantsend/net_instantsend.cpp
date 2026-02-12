// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <instantsend/net_instantsend.h>

#include <bls/bls_batchverifier.h>
#include <chainlock/chainlock.h>
#include <consensus/params.h>
#include <cxxtimer.hpp>
#include <instantsend/instantsend.h>
#include <llmq/commitment.h>
#include <llmq/quorumsman.h>
#include <llmq/signhash.h>
#include <llmq/signing.h>
#include <masternode/sync.h>
#include <node/interface_ui.h>
#include <util/thread.h>
#include <validation.h>

#include <chrono>
#include <set>

// Forward declaration to break dependency over node/transaction.h
namespace node {
CTransactionRef GetTransaction(const CBlockIndex* const block_index, const CTxMemPool* const m_mempool,
                               const uint256& hash, const Consensus::Params& consensusParams, uint256& hashBlock);
} // namespace node

using node::GetTransaction;
namespace {
constexpr int BATCH_VERIFIER_SOURCE_THRESHOLD{8};
constexpr int INVALID_ISLOCK_MISBEHAVIOR_SCORE{100};
constexpr int UNKNOWN_CYCLE_HASH_MISBEHAVIOR_SCORE{1};
constexpr int OLD_ACTIVE_SET_FAILURE_MISBEHAVIOR_SCORE{20};
constexpr auto WORK_THREAD_SLEEP_INTERVAL{std::chrono::milliseconds{100}};
} // namespace

static std::optional<int> GetBlockHeight(llmq::CInstantSendManager& is_manager, const CChainState& chainstate,
                                         const uint256& hash)
{
    if (hash.IsNull()) {
        return std::nullopt;
    }
    auto ret = is_manager.GetCachedHeight(hash);
    if (ret) return ret;

    const CBlockIndex* pindex = WITH_LOCK(::cs_main, return chainstate.m_blockman.LookupBlockIndex(hash));
    if (pindex == nullptr) {
        return std::nullopt;
    }
    is_manager.CacheBlockHeight(pindex);
    return pindex->nHeight;
}

struct NetInstantSend::BatchVerificationData {
    CBLSBatchVerifier<NodeId, uint256> batchVerifier{false, true, BATCH_VERIFIER_SOURCE_THRESHOLD};
    Uint256HashMap<llmq::CRecoveredSig> recSigs;
    size_t verifyCount{0};
    size_t alreadyVerified{0};
};

bool NetInstantSend::ValidateIncomingISLock(const instantsend::InstantSendLock& islock, NodeId node_id)
{
    if (!islock.TriviallyValid()) {
        m_peer_manager->PeerMisbehaving(node_id, INVALID_ISLOCK_MISBEHAVIOR_SCORE);
        return false;
    }

    return true;
}

std::optional<int> NetInstantSend::ResolveCycleHeight(const uint256& cycle_hash)
{
    auto cycle_height = GetBlockHeight(m_is_manager, m_chainstate, cycle_hash);
    if (cycle_height) {
        return cycle_height;
    }

    const auto block_index = WITH_LOCK(::cs_main, return m_chainstate.m_blockman.LookupBlockIndex(cycle_hash));
    if (block_index == nullptr) {
        return std::nullopt;
    }

    m_is_manager.CacheBlockHeight(block_index);
    return block_index->nHeight;
}

bool NetInstantSend::ValidateDeterministicCycleHeight(
    int cycle_height,
    const Consensus::LLMQParams& llmq_params,
    NodeId node_id)
{
    // Deterministic islocks MUST use rotation based llmq
    if (cycle_height % llmq_params.dkgInterval == 0) {
        return true;
    }

    m_peer_manager->PeerMisbehaving(node_id, INVALID_ISLOCK_MISBEHAVIOR_SCORE);
    return false;
}

std::unique_ptr<NetInstantSend::BatchVerificationData> NetInstantSend::BuildVerificationBatch(
    const Consensus::LLMQParams& llmq_params,
    int signOffset,
    const std::vector<instantsend::PendingISLockEntry>& pend)
{
    auto data = std::make_unique<BatchVerificationData>();

    for (const auto& pending : pend) {
        const auto& hash = pending.islock_hash;
        auto nodeId = pending.node_id;
        const auto& islock = pending.islock;

        if (data->batchVerifier.badSources.count(nodeId)) {
            continue;
        }

        CBLSSignature sig = islock->sig.Get();
        if (!sig.IsValid()) {
            data->batchVerifier.badSources.emplace(nodeId);
            continue;
        }

        auto id = islock->GetRequestId();

        // no need to verify an ISLOCK if we already have verified the recovered sig that belongs to it
        if (m_is_manager.Sigman().HasRecoveredSig(llmq_params.type, id, islock->txid)) {
            data->alreadyVerified++;
            continue;
        }

        auto cycleHeightOpt = GetBlockHeight(m_is_manager, m_chainstate, islock->cycleHash);
        if (!cycleHeightOpt) {
            data->batchVerifier.badSources.emplace(nodeId);
            continue;
        }

        int nSignHeight{-1};
        const auto dkgInterval = llmq_params.dkgInterval;
        const int tipHeight = m_is_manager.GetTipHeight();
        const int cycleHeight = *cycleHeightOpt;
        if (cycleHeight + dkgInterval < tipHeight) {
            nSignHeight = cycleHeight + dkgInterval - 1;
        }
        // For RegTest non-rotating quorum cycleHash has directly quorum hash
        auto quorum = llmq_params.useRotation ? llmq::SelectQuorumForSigning(llmq_params, m_chainstate.m_chain, m_qman,
                                                                             id, nSignHeight, signOffset)
                                              : m_qman.GetQuorum(llmq_params.type, islock->cycleHash);

        if (!quorum) {
            // should not happen, but if one fails to select, all others will also fail to select
            return nullptr;
        }
        uint256 signHash = llmq::SignHash{llmq_params.type, quorum->qc->quorumHash, id, islock->txid}.Get();
        data->batchVerifier.PushMessage(nodeId, hash, signHash, sig, quorum->qc->quorumPublicKey);
        data->verifyCount++;

        // We can reconstruct the CRecoveredSig objects from the islock and pass it to the signing manager, which
        // avoids unnecessary double-verification of the signature. We however only do this when verification here
        // turns out to be good (which is checked further down)
        if (!m_is_manager.Sigman().HasRecoveredSigForId(llmq_params.type, id)) {
            data->recSigs.try_emplace(hash, llmq::CRecoveredSig(llmq_params.type, quorum->qc->quorumHash, id, islock->txid,
                                                                islock->sig));
        }
    }

    return data;
}

Uint256HashSet NetInstantSend::ApplyVerificationResults(
    const Consensus::LLMQParams& llmq_params,
    bool ban,
    BatchVerificationData& data,
    const std::vector<instantsend::PendingISLockEntry>& pend)
{
    Uint256HashSet badISLocks;
    std::set<NodeId> penalized;

    for (const auto& pending : pend) {
        const auto& hash = pending.islock_hash;
        auto nodeId = pending.node_id;
        const auto& islock = pending.islock;

        const bool source_bad = data.batchVerifier.badSources.count(nodeId);
        const bool message_bad = data.batchVerifier.badMessages.count(hash);

        if (source_bad || message_bad) {
            LogPrint(BCLog::INSTANTSEND, "NetInstantSend::%s -- txid=%s, islock=%s: verification failed, peer=%d\n",
                     __func__, islock->txid.ToString(), hash.ToString(), nodeId);
            if (ban && source_bad && penalized.emplace(nodeId).second) {
                // Let's not be too harsh, as the peer might simply be unlucky and might have sent us
                // an old lock which does not validate anymore due to changed quorums
                m_peer_manager->PeerMisbehaving(nodeId, OLD_ACTIVE_SET_FAILURE_MISBEHAVIOR_SCORE);
            }
            if (message_bad) {
                badISLocks.emplace(hash);
            }
            continue;
        }

        ProcessInstantSendLock(nodeId, hash, islock);

        // Pass a reconstructed recovered sig to the signing manager to avoid double-verification of the sig.
        auto it = data.recSigs.find(hash);
        if (it != data.recSigs.end()) {
            auto recSig = std::make_shared<llmq::CRecoveredSig>(std::move(it->second));
            if (!m_is_manager.Sigman().HasRecoveredSigForId(llmq_params.type, recSig->getId())) {
                LogPrint(BCLog::INSTANTSEND, /* Continued */
                         "NetInstantSend::%s -- txid=%s, islock=%s: "
                         "passing reconstructed recSig to signing mgr, peer=%d\n",
                         __func__, islock->txid.ToString(), hash.ToString(), nodeId);
                m_is_manager.Sigman().PushReconstructedRecoveredSig(recSig);
            }
        }
    }

    return badISLocks;
}

void NetInstantSend::ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv)
{
    if (msg_type != NetMsgType::ISDLOCK) {
        return;
    }

    if (!m_is_manager.IsInstantSendEnabled()) return;

    auto islock = std::make_shared<instantsend::InstantSendLock>();
    vRecv >> *islock;

    const NodeId from = pfrom.GetId();
    uint256 hash = ::SerializeHash(*islock);

    WITH_LOCK(::cs_main, m_peer_manager->PeerEraseObjectRequest(from, CInv{MSG_ISDLOCK, hash}));

    if (!ValidateIncomingISLock(*islock, from)) {
        return;
    }

    auto cycle_height = ResolveCycleHeight(islock->cycleHash);
    if (!cycle_height) {
        // Maybe we don't have the block yet or maybe some peer spams invalid values for cycleHash
        m_peer_manager->PeerMisbehaving(from, UNKNOWN_CYCLE_HASH_MISBEHAVIOR_SCORE);
        return;
    }

    auto llmqType = Params().GetConsensus().llmqTypeDIP0024InstantSend;
    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    assert(llmq_params_opt);
    if (!ValidateDeterministicCycleHeight(*cycle_height, *llmq_params_opt, from)) {
        return;
    }

    if (!m_is_manager.AlreadyHave(CInv{MSG_ISDLOCK, hash})) {
        LogPrint(BCLog::INSTANTSEND, "NetInstantSend -- ISDLOCK txid=%s, islock=%s: received islock, peer=%d\n",
                 islock->txid.ToString(), hash.ToString(), from);

        m_is_manager.EnqueueInstantSendLock(from, hash, std::move(islock));
    }
}

void NetInstantSend::Start()
{
    // can't start new thread if we have one running already
    if (workThread.joinable()) {
        assert(false);
    }

    workThread = std::thread(&util::TraceThread, "isman", [this] { WorkThreadMain(); });
}

void NetInstantSend::Stop()
{
    // make sure to call Interrupt() first
    if (!workInterrupt) {
        assert(false);
    }

    if (workThread.joinable()) {
        workThread.join();
    }
}

Uint256HashSet NetInstantSend::ProcessPendingInstantSendLocks(
    const Consensus::LLMQParams& llmq_params, int signOffset, bool ban,
    const std::vector<instantsend::PendingISLockEntry>& pend)
{
    auto batch = BuildVerificationBatch(llmq_params, signOffset, pend);
    if (!batch) return {};

    cxxtimer::Timer verifyTimer(true);
    batch->batchVerifier.Verify();
    verifyTimer.stop();

    LogPrint(BCLog::INSTANTSEND, "NetInstantSend::%s -- verified locks. count=%d, alreadyVerified=%d, vt=%d, nodes=%d\n",
             __func__, batch->verifyCount, batch->alreadyVerified,
             verifyTimer.count(), batch->batchVerifier.GetUniqueSourceCount());

    return ApplyVerificationResults(llmq_params, ban, *batch, pend);
}

void NetInstantSend::ProcessPendingISLocks(std::vector<instantsend::PendingISLockEntry>&& locks_to_process)
{
    // TODO Investigate if leaving this is ok
    auto llmqType = Params().GetConsensus().llmqTypeDIP0024InstantSend;
    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    assert(llmq_params_opt);
    const auto& llmq_params = llmq_params_opt.value();
    auto dkgInterval = llmq_params.dkgInterval;

    // First check against the current active set and don't ban
    auto bad_is_locks = ProcessPendingInstantSendLocks(llmq_params, /*signOffset=*/0, /*ban=*/false, locks_to_process);
    if (!bad_is_locks.empty()) {
        LogPrint(BCLog::INSTANTSEND, "NetInstantSend::%s -- doing verification on old active set\n", __func__);

        // filter out valid IS locks from "pend" - keep only bad ones
        std::vector<instantsend::PendingISLockEntry> still_pending;
        still_pending.reserve(bad_is_locks.size());
        for (auto& pending : locks_to_process) {
            if (bad_is_locks.contains(pending.islock_hash)) {
                still_pending.emplace_back(std::move(pending));
            }
        }
        // Now check against the previous active set and perform banning if this fails
        ProcessPendingInstantSendLocks(llmq_params, dkgInterval, /*ban=*/true, still_pending);
    }
    uiInterface.NotifyInstantSendChanged();
}

void NetInstantSend::ProcessInstantSendLock(NodeId from, const uint256& hash, const instantsend::InstantSendLockPtr& islock)
{
    LogPrint(BCLog::INSTANTSEND, "NetSigning::%s -- txid=%s, islock=%s: processing islock, peer=%d\n", __func__,
             islock->txid.ToString(), hash.ToString(), from);

    if (auto signer = m_is_manager.Signer(); signer) {
        signer->ClearLockFromQueue(islock);
    }
    if (!m_is_manager.PreVerifyIsLock(hash, islock, from)) return;

    uint256 hashBlock{};
    auto tx = GetTransaction(nullptr, &m_mempool, islock->txid, Params().GetConsensus(), hashBlock);
    const bool found_transaction{tx != nullptr};
    // we ignore failure here as we must be able to propagate the lock even if we don't have the TX locally
    std::optional<int> minedHeight = GetBlockHeight(m_is_manager, m_chainstate, hashBlock);
    if (found_transaction) {
        if (!minedHeight.has_value()) {
            const CBlockIndex* pindexMined = WITH_LOCK(::cs_main,
                                                       return m_chainstate.m_blockman.LookupBlockIndex(hashBlock));
            if (pindexMined != nullptr) {
                m_is_manager.CacheBlockHeight(pindexMined);
                minedHeight = pindexMined->nHeight;
            }
        }
        // Let's see if the TX that was locked by this islock is already mined in a ChainLocked block. If yes,
        // we can simply ignore the islock, as the ChainLock implies locking of all TXs in that chain
        if (minedHeight.has_value() && m_is_manager.Chainlocks().HasChainLock(*minedHeight, hashBlock)) {
            LogPrint(BCLog::INSTANTSEND, /* Continued */
                     "NetSigning::%s -- txlock=%s, islock=%s: dropping islock as it already got a "
                     "ChainLock in block %s, peer=%d\n",
                     __func__, islock->txid.ToString(), hash.ToString(), hashBlock.ToString(), from);
            return;
        }
        m_is_manager.WriteNewISLock(hash, islock, minedHeight);
    } else {
        m_is_manager.AddPendingISLock(hash, islock, from);
    }


    // This will also add children TXs to pendingRetryTxs
    m_is_manager.RemoveNonLockedTx(islock->txid, true);
    // We don't need the recovered sigs for the inputs anymore. This prevents unnecessary propagation of these sigs.
    // We only need the ISLOCK from now on to detect conflicts
    m_is_manager.TruncateRecoveredSigsForInputs(*islock);
    ResolveBlockConflicts(hash, *islock);

    if (found_transaction) {
        RemoveMempoolConflictsForLock(hash, *islock);
        LogPrint(BCLog::INSTANTSEND, "NetSigning::%s -- notify about lock %s for tx %s\n", __func__, hash.ToString(),
                 tx->GetHash().ToString());
        GetMainSignals().NotifyTransactionLock(tx, islock);
        // bump m_mempool counter to make sure newly locked txes are picked up by getblocktemplate
        m_mempool.AddTransactionsUpdated(1);
    }

    CInv inv(MSG_ISDLOCK, hash);
    if (found_transaction) {
        m_peer_manager->PeerRelayInvFiltered(inv, *tx);
    } else {
        m_peer_manager->PeerRelayInvFiltered(inv, islock->txid);
        m_peer_manager->PeerAskPeersForTransaction(islock->txid);
    }
}

void NetInstantSend::WorkThreadMain()
{
    while (!workInterrupt) {
        bool fMoreWork = [&]() -> bool {
            if (!m_is_manager.IsInstantSendEnabled()) return false;

            auto [more_work, locks] = m_is_manager.FetchPendingLocks();
            if (!locks.empty()) {
                ProcessPendingISLocks(std::move(locks));
            }
            if (auto signer = m_is_manager.Signer(); signer) {
                signer->ProcessPendingRetryLockTxs(m_is_manager.PrepareTxToRetry());
            }
            return more_work;
        }();

        if (!fMoreWork && !workInterrupt.sleep_for(WORK_THREAD_SLEEP_INTERVAL)) {
            return;
        }
    }
}

void NetInstantSend::TransactionAddedToMempool(const CTransactionRef& tx, int64_t, uint64_t mempool_sequence)
{
    if (!m_is_manager.IsInstantSendEnabled() || !m_mn_sync.IsBlockchainSynced() || tx->vin.empty()) {
        return;
    }

    instantsend::InstantSendLockPtr islock = m_is_manager.AttachISLockToTx(tx);
    if (islock == nullptr) {
        if (auto signer = m_is_manager.Signer(); signer) {
            signer->ProcessTx(*tx, false, Params().GetConsensus());
        }
        // TX is not locked, so make sure it is tracked
        m_is_manager.AddNonLockedTx(tx, nullptr);
    } else {
        RemoveMempoolConflictsForLock(::SerializeHash(*islock), *islock);
    }
}

void NetInstantSend::RemoveMempoolConflictsForLock(const uint256& hash, const instantsend::InstantSendLock& islock)
{
    Uint256HashMap<CTransactionRef> toDelete;

    {
        LOCK(m_mempool.cs);

        for (const auto& in : islock.inputs) {
            auto it = m_mempool.mapNextTx.find(in);
            if (it == m_mempool.mapNextTx.end()) {
                continue;
            }
            if (it->second->GetHash() != islock.txid) {
                toDelete.emplace(it->second->GetHash(), m_mempool.get(it->second->GetHash()));

                LogPrintf("%s -- txid=%s, mempool TX %s with input %s conflicts with islock=%s\n", __func__,
                          islock.txid.ToString(), it->second->GetHash().ToString(), in.ToStringShort(), hash.ToString());
            }
        }

        for (const auto& p : toDelete) {
            m_mempool.removeRecursive(*p.second, MemPoolRemovalReason::CONFLICT);
        }
    }

    for (const auto& p : toDelete) {
        m_is_manager.RemoveConflictedTx(*p.second);
    }
}

void NetInstantSend::SynchronousUpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork,
                                                bool fInitialDownload)
{
    m_is_manager.CacheTipHeight(pindexNew);
}

void NetInstantSend::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    bool fDIP0008Active = pindexNew->pprev && pindexNew->pprev->nHeight >= Params().GetConsensus().DIP0008Height;

    if (m_is_manager.Chainlocks().IsEnabled() && fDIP0008Active) {
        // Nothing to do here. We should keep all islocks and let chainlocks handle them.
        return;
    }

    int nConfirmedHeight = pindexNew->nHeight - Params().GetConsensus().nInstantSendKeepLock;
    const CBlockIndex* pindex = pindexNew->GetAncestor(nConfirmedHeight);

    if (pindex) {
        m_is_manager.HandleFullyConfirmedBlock(pindex);
    }
}

void NetInstantSend::TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason reason,
                                                   uint64_t mempool_sequence)
{
    m_is_manager.TransactionIsRemoved(tx);
}

void NetInstantSend::BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex)
{
    if (!m_is_manager.IsInstantSendEnabled()) {
        return;
    }

    m_is_manager.CacheTipHeight(pindex);

    if (m_mn_sync.IsBlockchainSynced()) {
        const bool has_chainlock = m_is_manager.Chainlocks().HasChainLock(pindex->nHeight, pindex->GetBlockHash());
        for (const auto& tx : pblock->vtx) {
            if (tx->IsCoinBase() || tx->vin.empty()) {
                // coinbase and TXs with no inputs can't be locked
                continue;
            }

            if (!m_is_manager.IsLocked(tx->GetHash()) && !has_chainlock) {
                if (auto signer = m_is_manager.Signer(); signer) {
                    signer->ProcessTx(*tx, true, Params().GetConsensus());
                }
                // TX is not locked, so make sure it is tracked
                m_is_manager.AddNonLockedTx(tx, pindex);
            } else {
                // TX is locked, so make sure we don't track it anymore
                m_is_manager.RemoveNonLockedTx(tx->GetHash(), true);
            }
        }
    }
    m_is_manager.WriteBlockISLocks(pblock, pindex);
}

void NetInstantSend::BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindexDisconnected)
{
    m_is_manager.CacheDisconnectBlock(pindexDisconnected);
    m_is_manager.CacheTipHeight(pindexDisconnected->pprev);
    m_is_manager.RemoveBlockISLocks(pblock, pindexDisconnected);
}

void NetInstantSend::NotifyChainLock(const CBlockIndex* pindex, const std::shared_ptr<const chainlock::ChainLockSig>& clsig)
{
    m_is_manager.HandleFullyConfirmedBlock(pindex);
}

void NetInstantSend::ResolveBlockConflicts(const uint256& islockHash, const instantsend::InstantSendLock& islock)
{
    auto conflicts = m_is_manager.RetrieveISConflicts(islockHash, islock);

    // Lets see if any of the conflicts was already mined into a ChainLocked block
    bool hasChainLockedConflict = false;
    for (const auto& p : conflicts) {
        const auto* pindex = p.first;
        if (m_is_manager.Chainlocks().HasChainLock(pindex->nHeight, pindex->GetBlockHash())) {
            hasChainLockedConflict = true;
            break;
        }
    }

    // If a conflict was mined into a ChainLocked block, then we have no other choice and must prune the ISLOCK and all
    // chained ISLOCKs that build on top of this one. The probability of this is practically zero and can only happen
    // when large parts of the masternode network are controlled by an attacker. In this case we must still find
    // consensus and its better to sacrifice individual ISLOCKs then to sacrifice whole ChainLocks.
    if (hasChainLockedConflict) {
        LogPrintf("NetInstantSend::%s -- txid=%s, islock=%s: at least one conflicted TX already got a ChainLock\n",
                  __func__, islock.txid.ToString(), islockHash.ToString());
        m_is_manager.RemoveConflictingLock(islockHash, islock);
        return;
    }

    bool isLockedTxKnown = m_is_manager.IsKnownTx(islockHash);

    bool activateBestChain = false;
    for (const auto& p : conflicts) {
        const auto* pindex = p.first;
        for (const auto& p2 : p.second) {
            const auto& tx = *p2.second;
            m_is_manager.RemoveConflictedTx(tx);
        }

        LogPrintf("NetInstantSend::%s -- invalidating block %s\n", __func__, pindex->GetBlockHash().ToString());

        BlockValidationState state;
        // need non-const pointer
        auto pindex2 = WITH_LOCK(::cs_main, return m_chainstate.m_blockman.LookupBlockIndex(pindex->GetBlockHash()));
        if (!m_chainstate.InvalidateBlock(state, pindex2)) {
            LogPrintf("NetInstantSend::%s -- InvalidateBlock failed: %s\n", __func__, state.ToString());
            // This should not have happened and we are in a state were it's not safe to continue anymore
            assert(false);
        }
        if (isLockedTxKnown) {
            activateBestChain = true;
        } else {
            LogPrintf("NetInstantSend::%s -- resetting block %s\n", __func__, pindex2->GetBlockHash().ToString());
            LOCK(::cs_main);
            m_chainstate.ResetBlockFailureFlags(pindex2);
        }
    }

    if (activateBestChain) {
        BlockValidationState state;
        if (!m_chainstate.ActivateBestChain(state)) {
            LogPrintf("NetInstantSend::%s -- ActivateBestChain failed: %s\n", __func__, state.ToString());
            // This should not have happened and we are in a state were it's not safe to continue anymore
            assert(false);
        }
    }
}
