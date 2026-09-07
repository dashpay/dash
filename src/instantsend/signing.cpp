// Copyright (c) 2019-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <instantsend/signing.h>

#include <chain.h>
#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <evo/assetlocktx.h>
#include <evo/chainhelper.h>
#include <evo/creditpool.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <index/txindex.h>
#include <instantsend/instantsend.h>
#include <llmq/quorumsman.h>
#include <llmq/signing_shares.h>
#include <logging.h>
#include <masternode/sync.h>
#include <spork.h>
#include <util/helpers.h>
#include <validation.h>

#include <ranges>

// Forward declaration to break dependency over node/transaction.h
namespace node {
CTransactionRef GetTransaction(const CBlockIndex* const block_index, const CTxMemPool* const mempool,
                               const uint256& hash, const Consensus::Params& consensusParams, uint256& hashBlock);
} // namespace node

using node::fImporting;
using node::fReindex;
using node::GetTransaction;

namespace instantsend {
InstantSendSigner::InstantSendSigner(const ChainstateManager& chainman, const chainlock::Chainlocks& chainlocks,
                                     llmq::CInstantSendManager& isman, llmq::CSigningManager& sigman,
                                     llmq::CSigSharesManager& shareman, llmq::CQuorumManager& qman,
                                     CSporkManager& sporkman, CTxMemPool& mempool, const CMasternodeSync& mn_sync) :
    m_chainman{chainman},
    m_chainlocks{chainlocks},
    m_isman{isman},
    m_sigman{sigman},
    m_shareman{shareman},
    m_qman{qman},
    m_sporkman{sporkman},
    m_mempool{mempool},
    m_mn_sync{mn_sync}
{
}

InstantSendSigner::~InstantSendSigner() = default;

void InstantSendSigner::RegisterRecoveryInterface()
{
    m_sigman.RegisterRecoveredSigsListener(this);
}

void InstantSendSigner::UnregisterRecoveryInterface()
{
    m_sigman.UnregisterRecoveredSigsListener(this);
}

void InstantSendSigner::ClearInputsFromQueue(const Uint256HashSet& ids)
{
    LOCK(cs_input_requests);
    for (const auto& id : ids) {
        inputRequestIds.erase(id);
    }
}

void InstantSendSigner::ClearLockFromQueue(const InstantSendLockPtr& islock)
{
    LOCK(cs_creating);
    creatingInstantSendLocks.erase(islock->GetRequestId());
    txToCreatingInstantSendLocks.erase(islock->txid);
}

llmq::RecoveredSigResult InstantSendSigner::HandleNewRecoveredSig(const llmq::CRecoveredSig& recoveredSig)
{
    if (!m_isman.IsInstantSendEnabled()) {
        return std::monostate{};
    }

    if (Params().GetConsensus().llmqTypeDIP0024InstantSend == Consensus::LLMQType::LLMQ_NONE) {
        return std::monostate{};
    }

    uint256 txid;
    if (LOCK(cs_input_requests); inputRequestIds.count(recoveredSig.getId())) {
        txid = recoveredSig.getMsgHash();
    }
    if (!txid.IsNull()) {
        HandleNewInputLockRecoveredSig(recoveredSig, txid);
    } else if (/*isInstantSendLock=*/WITH_LOCK(cs_creating, return creatingInstantSendLocks.count(recoveredSig.getId()))) {
        HandleNewInstantSendLockRecoveredSig(recoveredSig);
    }
    return std::monostate{};
}

bool InstantSendSigner::IsInstantSendMempoolSigningEnabled() const
{
    return !fReindex && !fImporting && m_sporkman.GetSporkValue(SPORK_2_INSTANTSEND_ENABLED) == 0;
}

void InstantSendSigner::HandleNewInputLockRecoveredSig(const llmq::CRecoveredSig& recoveredSig, const uint256& txid)
{
    if (g_txindex) {
        g_txindex->BlockUntilSyncedToCurrentChain();
    }

    uint256 _hashBlock{};
    const auto tx = GetTransaction(nullptr, &m_mempool, txid, Params().GetConsensus(), _hashBlock);
    if (!tx) {
        return;
    }

    if (LogAcceptDebug(BCLog::INSTANTSEND)) {
        for (const auto& outpoint : GetLockInputs(*tx)) {
            if (GenInputLockRequestId(outpoint) == recoveredSig.getId()) {
                LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: got recovered sig for input %s\n", __func__,
                         txid.ToString(), outpoint.ToStringShort());
                break;
            }
        }
    }

    TrySignInstantSendLock(*tx);
}

void InstantSendSigner::ProcessPendingRetryLockTxs(const std::vector<CTransactionRef>& retryTxs)
{
    if (!m_isman.IsInstantSendEnabled()) {
        return;
    }

    int retryCount = 0;
    for (const auto& tx : retryTxs) {
        {
            if (LOCK(cs_creating); txToCreatingInstantSendLocks.count(tx->GetHash())) {
                // we're already in the middle of locking this one
                continue;
            }
            if (m_isman.IsLocked(tx->GetHash())) {
                continue;
            }
            if (m_isman.GetConflictingLock(*tx) != nullptr) {
                // should not really happen as we have already filtered these out
                continue;
            }
        }

        // CheckCanLock is already called by ProcessTx, so we should avoid calling it twice. But we also shouldn't spam
        // the logs when retrying TXs that are not ready yet.
        if (LogAcceptDebug(BCLog::INSTANTSEND)) {
            if (!CheckCanLock(*tx, false, Params().GetConsensus())) {
                continue;
            }
            LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: retrying to lock\n", __func__, tx->GetHash().ToString());
        }

        ProcessTx(*tx, false, Params().GetConsensus());
        retryCount++;
    }

    if (retryCount != 0) {
        LogPrint(BCLog::INSTANTSEND, "%s -- retried %d TXs.\n", __func__, retryCount);
    }
}

bool InstantSendSigner::CheckCanLock(const CTransaction& tx, bool printDebug, const Consensus::Params& params) const
{
    if (tx.IsPlatformTransfer()) {
        return CheckCanLockAssetUnlock(tx, printDebug);
    }
    if (tx.vin.empty()) {
        // can't lock TXs without inputs (e.g. quorum commitments)
        return false;
    }

    return std::ranges::all_of(tx.vin, [&](const auto& in) {
        return CheckCanLock(in.prevout, printDebug, tx.GetHash(), params);
    });
}

bool InstantSendSigner::CheckCanLockAssetUnlock(const CTransaction& tx, bool printDebug) const
{
    const auto log_refusal = [&](std::string_view reason) {
        if (printDebug) {
            LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: asset unlock not lockable: %s\n", __func__,
                     tx.GetHash().ToString(), reason);
        }
        return false;
    };
    // Version 1 instances change txid when Platform re-signs them, so a lock on one would not
    // survive a re-sign; only stable-txid instances are locked
    if (!IsAssetUnlockWithStableTxid(tx)) return log_refusal("version 1");
    const auto opt_payload = GetTxPayload<CAssetUnlockPayload>(tx);
    if (!opt_payload) return log_refusal("bad payload");

    LOCK2(::cs_main, m_mempool.cs);
    if (!m_mempool.exists(tx.GetHash())) return log_refusal("not in mempool");
    // A withdrawal signed as version 1 before v24 activation may be re-signed as version 2 after
    // it; both instances then claim the index under different txids. Locking the version 2
    // instance while the version 1 one is minable risks the lock losing to a ChainLock.
    if (m_mempool.GetAssetUnlockTxidsByIndex(opt_payload->getIndex()).size() != 1) {
        return log_refusal("another instance of this withdrawal index is in the mempool");
    }

    Chainstate& chainstate = m_chainman.ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    // Minable in the next block: inside its height window and signed by a recent quorum
    TxValidationState state;
    const bool is_v24_active{DeploymentActiveAfter(tip, m_chainman, Consensus::DEPLOYMENT_V24)};
    if (!chainstate.ChainHelper().special_tx->CheckSpecialTx(tx, tip, is_v24_active, chainstate.CoinsTip(),
                                                             /*check_sigs=*/true, state)) {
        return log_refusal(state.ToString());
    }
    // Fits the withdrawal limit alongside every other pending withdrawal: the limit is enforced
    // only when a block is connected, so an over-limit unlock is otherwise indistinguishable from
    // a minable one in the mempool. Platform pools withdrawals under the same daily limit, so
    // the pending total exceeding it means something is wrong and nothing is locked until the
    // window clears rather than guessing which withdrawals miners will pick.
    const CCreditPool pool = chainstate.ChainHelper().GetCreditPool(tip);
    if (const CAmount pending{m_mempool.GetPendingAssetUnlockAmount()}; pending > pool.currentLimit) {
        return log_refusal(strprintf("pending withdrawals %d exceed the credit pool limit %d", pending, pool.currentLimit));
    }
    return true;
}

bool InstantSendSigner::CheckCanLock(const COutPoint& outpoint, bool printDebug, const uint256& txHash,
                                     const Consensus::Params& params) const
{
    int nInstantSendConfirmationsRequired = params.nInstantSendConfirmationsRequired;

    if (m_isman.IsLocked(outpoint.hash)) {
        // if prevout was ix locked, allow locking of descendants (no matter if prevout is in mempool or already mined)
        return true;
    }

    auto mempoolTx = m_mempool.get(outpoint.hash);
    if (mempoolTx) {
        if (printDebug) {
            LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: parent mempool TX %s is not locked\n", __func__,
                     txHash.ToString(), outpoint.hash.ToString());
        }
        return false;
    }

    uint256 hashBlock{};
    const auto tx = GetTransaction(nullptr, &m_mempool, outpoint.hash, params, hashBlock);
    // this relies on enabled txindex and won't work if we ever try to remove the requirement for txindex for masternodes
    if (!tx || hashBlock.IsNull()) {
        if (printDebug) {
            LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: failed to find parent TX %s in mined block\n", __func__, txHash.ToString(),
                     outpoint.hash.ToString());
        }
        return false;
    }

    int blockHeight{0};
    if (auto ret = m_isman.GetCachedHeight(hashBlock)) {
        blockHeight = *ret;
    } else {
        const CBlockIndex* pindex = WITH_LOCK(::cs_main, return m_chainman.m_blockman.LookupBlockIndex(hashBlock));
        if (pindex == nullptr) {
            if (printDebug) {
                LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: failed to determine mined height for parent TX %s\n",
                         __func__, txHash.ToString(), outpoint.hash.ToString());
            }
            return false;
        }
        m_isman.CacheBlockHeight(pindex);
        blockHeight = pindex->nHeight;
    }

    const int tipHeight = m_isman.GetTipHeight();

    if (tipHeight < blockHeight) {
        if (printDebug) {
            LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: cached tip height %d is below block height %d for parent TX %s\n",
                     __func__, txHash.ToString(), tipHeight, blockHeight, outpoint.hash.ToString());
        }
        return false;
    }

    const int nTxAge = tipHeight - blockHeight + 1;

    if (nTxAge < nInstantSendConfirmationsRequired && !m_chainlocks.HasChainLock(blockHeight, hashBlock)) {
        if (printDebug) {
            LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: outpoint %s too new and not ChainLocked. nTxAge=%d, nInstantSendConfirmationsRequired=%d\n", __func__,
                     txHash.ToString(), outpoint.ToStringShort(), nTxAge, nInstantSendConfirmationsRequired);
        }
        return false;
    }

    return true;
}

void InstantSendSigner::HandleNewInstantSendLockRecoveredSig(const llmq::CRecoveredSig& recoveredSig)
{
    InstantSendLockPtr islock;

    {
        LOCK(cs_creating);
        auto it = creatingInstantSendLocks.find(recoveredSig.getId());
        if (it == creatingInstantSendLocks.end()) {
            return;
        }

        islock = std::make_shared<InstantSendLock>(std::move(it->second));
        creatingInstantSendLocks.erase(it);
        txToCreatingInstantSendLocks.erase(islock->txid);
    }

    if (islock->txid != recoveredSig.getMsgHash()) {
        LogPrintf("%s -- txid=%s: islock conflicts with %s, dropping own version\n", __func__, islock->txid.ToString(),
                  recoveredSig.getMsgHash().ToString());
        return;
    }

    islock->sig = recoveredSig.sig;
    m_isman.TryEmplacePendingLock(/*hash=*/::SerializeHash(*islock), /*id=*/-1, islock);
}

void InstantSendSigner::ProcessTx(const CTransaction& tx, bool fRetroactive, const Consensus::Params& params)
{
    if (!m_isman.IsInstantSendEnabled() || !m_mn_sync.IsBlockchainSynced()) {
        return;
    }

    if (params.llmqTypeDIP0024InstantSend == Consensus::LLMQType::LLMQ_NONE) {
        return;
    }

    if (!CheckCanLock(tx, true, params)) {
        LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: CheckCanLock returned false\n", __func__, tx.GetHash().ToString());
        return;
    }

    auto conflictingLock = m_isman.GetConflictingLock(tx);
    if (conflictingLock != nullptr) {
        auto conflictingLockHash = ::SerializeHash(*conflictingLock);
        LogPrintf("%s -- txid=%s: conflicts with islock %s, txid=%s\n", __func__, tx.GetHash().ToString(),
                  conflictingLockHash.ToString(), conflictingLock->txid.ToString());
        return;
    }

    // Only sign for inlocks or islocks if mempool IS signing is enabled.
    // However, if we are processing a tx because it was included in a block we should
    // sign even if mempool IS signing is disabled. This allows a ChainLock to happen on this
    // block after we retroactively locked all transactions.
    if (!IsInstantSendMempoolSigningEnabled() && !fRetroactive) return;

    if (!TrySignInputLocks(tx, fRetroactive, params.llmqTypeDIP0024InstantSend, params)) {
        return;
    }

    // We might have received all input locks before we got the corresponding TX. In this case, we have to sign the
    // islock now instead of waiting for the input locks.
    TrySignInstantSendLock(tx);
}

bool InstantSendSigner::TrySignInputLocks(const CTransaction& tx, bool fRetroactive, Consensus::LLMQType llmqType,
                                          const Consensus::Params& params)
{
    const std::vector<COutPoint> inputs{GetLockInputs(tx)};
    std::vector<uint256> ids;
    ids.reserve(inputs.size());

    size_t alreadyVotedCount = 0;
    for (const auto& outpoint : inputs) {
        auto id = GenInputLockRequestId(outpoint);
        ids.emplace_back(id);

        uint256 otherTxHash;
        if (m_sigman.GetVoteForId(params.llmqTypeDIP0024InstantSend, id, otherTxHash)) {
            if (otherTxHash != tx.GetHash()) {
                LogPrintf("%s -- txid=%s: input %s is conflicting with previous vote for tx %s\n", __func__,
                          tx.GetHash().ToString(), outpoint.ToStringShort(), otherTxHash.ToString());
                return false;
            }
            alreadyVotedCount++;
        }

        // don't even try the actual signing if any input is conflicting
        if (m_sigman.IsConflicting(params.llmqTypeDIP0024InstantSend, id, tx.GetHash())) {
            LogPrintf("%s -- txid=%s: m_sigman.IsConflicting returned true. id=%s\n", __func__, tx.GetHash().ToString(),
                      id.ToString());
            return false;
        }
    }
    if (!fRetroactive && alreadyVotedCount == ids.size()) {
        LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: already voted on all inputs, bailing out\n", __func__,
                 tx.GetHash().ToString());
        return true;
    }

    LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: trying to vote on %d inputs\n", __func__, tx.GetHash().ToString(),
             inputs.size());

    for (const auto i : util::irange(inputs.size())) {
        const auto& outpoint = inputs[i];
        auto& id = ids[i];
        WITH_LOCK(cs_input_requests, inputRequestIds.emplace(id));
        LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: trying to vote on input %s with id %s. fRetroactive=%d\n",
                 __func__, tx.GetHash().ToString(), outpoint.ToStringShort(), id.ToString(), fRetroactive);
        if (m_shareman.AsyncSignIfMember(llmqType, id, tx.GetHash(), {}, fRetroactive)) {
            LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: voted on input %s with id %s\n", __func__,
                     tx.GetHash().ToString(), outpoint.ToStringShort(), id.ToString());
        }
    }

    return true;
}

void InstantSendSigner::TrySignInstantSendLock(const CTransaction& tx)
{
    const auto llmqType = Params().GetConsensus().llmqTypeDIP0024InstantSend;

    InstantSendLock islock;
    islock.txid = tx.GetHash();
    islock.inputs = GetLockInputs(tx);

    for (const auto& outpoint : islock.inputs) {
        auto id = GenInputLockRequestId(outpoint);
        if (!m_sigman.HasRecoveredSig(llmqType, id, tx.GetHash())) {
            return;
        }
    }

    LogPrint(BCLog::INSTANTSEND, "%s -- txid=%s: got all recovered sigs, creating InstantSendLock\n", __func__,
             tx.GetHash().ToString());

    auto id = islock.GetRequestId();

    if (m_sigman.HasRecoveredSigForId(llmqType, id)) {
        return;
    }

    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    assert(llmq_params_opt);
    const CChain& active_chain = *WITH_LOCK(::cs_main, return &m_chainman.ActiveChain());
    const auto quorum = llmq::SelectQuorumForSigning(llmq_params_opt.value(), active_chain, m_qman, id);

    if (!quorum) {
        LogPrint(BCLog::INSTANTSEND, "%s -- failed to select quorum. islock id=%s, txid=%s\n", __func__, id.ToString(),
                 tx.GetHash().ToString());
        return;
    }

    const int cycle_height = quorum->m_quorum_base_block_index->nHeight -
                             quorum->m_quorum_base_block_index->nHeight % llmq_params_opt->dkgInterval;
    islock.cycleHash = quorum->m_quorum_base_block_index->GetAncestor(cycle_height)->GetBlockHash();

    {
        LOCK(cs_creating);
        auto e = creatingInstantSendLocks.emplace(id, std::move(islock));
        if (!e.second) {
            return;
        }
        txToCreatingInstantSendLocks.emplace(tx.GetHash(), &e.first->second);
    }

    m_shareman.AsyncSignIfMember(llmqType, id, tx.GetHash(), quorum->m_quorum_base_block_index->GetBlockHash());
}
} // namespace instantsend
