// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinjoin/server.h>

#include <active/masternode.h>
#include <evo/deterministicmns.h>
#include <masternode/meta.h>
#include <masternode/sync.h>

#include <core_io.h>
#include <net.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <scheduler.h>
#include <script/interpreter.h>
#include <serialize.h>
#include <shutdown.h>
#include <streams.h>
#include <txmempool.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <validation.h>

#include <univalue.h>

#include <ranges>

CCoinJoinServer::CCoinJoinServer(PeerManagerInternal* peer_manager, ChainstateManager& chainman, CConnman& _connman,
                                 CDeterministicMNManager& dmnman, CDSTXManager& dstxman, CMasternodeMetaMan& mn_metaman,
                                 CTxMemPool& mempool, const CActiveMasternodeManager& mn_activeman,
                                 const CMasternodeSync& mn_sync, const llmq::CInstantSendManager& isman) :
    NetHandler(peer_manager),
    m_chainman{chainman},
    connman{_connman},
    m_dmnman{dmnman},
    m_dstxman{dstxman},
    m_mn_metaman{mn_metaman},
    mempool{mempool},
    m_mn_activeman{mn_activeman},
    m_mn_sync{mn_sync},
    m_isman{isman},
    fUnitTest{false}
{
}

CCoinJoinServer::~CCoinJoinServer() = default;

void CCoinJoinServer::ProcessMessage(CNode& peer, const std::string& msg_type, CDataStream& vRecv)
{
    if (!m_mn_sync.IsBlockchainSynced()) return;

    if (msg_type == NetMsgType::DSACCEPT) {
        ProcessDSACCEPT(peer, vRecv);
    } else if (msg_type == NetMsgType::DSQUEUE) {
        ProcessDSQUEUE(peer.GetId(), vRecv);
    } else if (msg_type == NetMsgType::DSVIN) {
        ProcessDSVIN(peer, vRecv);
    } else if (msg_type == NetMsgType::DSSIGNFINALTX) {
        ProcessDSSIGNFINALTX(peer, vRecv);
    }
}

void CCoinJoinServer::ProcessDSACCEPT(CNode& peer, CDataStream& vRecv)
{
    assert(m_mn_metaman.IsValid());

    if (WITH_LOCK(cs_coinjoin, return IsSessionReady())) {
        // too many users in this session already, reject new ones
        LogPrint(BCLog::COINJOIN, "DSACCEPT -- queue is already full!\n");
        PushStatus(peer, STATUS_REJECTED, ERR_QUEUE_FULL);
        return;
    }

    CCoinJoinAccept dsa;
    vRecv >> dsa;

    LogPrint(BCLog::COINJOIN, "DSACCEPT -- nDenom %d (%s)  txCollateral %s", dsa.nDenom, CoinJoin::DenominationToString(dsa.nDenom), dsa.txCollateral.ToString()); /* Continued */

    auto mnList = m_dmnman.GetListAtChainTip();
    auto dmn = mnList.GetValidMNByCollateral(m_mn_activeman.GetOutPoint());
    if (!dmn) {
        PushStatus(peer, STATUS_REJECTED, ERR_MN_LIST);
        return;
    }

    if (WITH_LOCK(cs_coinjoin, return m_session_collaterals.empty())) {
        {
            const auto hasQueue = m_queueman.TryHasQueueFromMasternode(m_mn_activeman.GetOutPoint());
            if (!hasQueue.has_value()) return;
            if (*hasQueue) {
                // refuse to create another queue this often
                LogPrint(BCLog::COINJOIN, "DSACCEPT -- last dsq is still in queue, refuse to mix\n");
                PushStatus(peer, STATUS_REJECTED, ERR_RECENT);
                return;
            }
        }

        if (m_mn_metaman.IsMixingThresholdExceeded(dmn->proTxHash, mnList.GetCounts().enabled())) {
            if (fLogIPs) {
                LogPrint(BCLog::COINJOIN, "DSACCEPT -- last dsq too recent, must wait: peer=%d, addr=%s\n",
                         peer.GetId(), peer.addr.ToStringAddrPort());
            } else {
                LogPrint(BCLog::COINJOIN, "DSACCEPT -- last dsq too recent, must wait: peer=%d\n", peer.GetId());
            }
            PushStatus(peer, STATUS_REJECTED, ERR_RECENT);
            return;
        }
    }

    PoolMessage nMessageID = MSG_NOERR;

    bool fResult = nSessionID == 0 ? CreateNewSession(dsa, nMessageID)
            : AddUserToExistingSession(dsa, nMessageID);
    if (fResult) {
        LogPrint(BCLog::COINJOIN, "DSACCEPT -- is compatible, please submit!\n");
        PushStatus(peer, STATUS_ACCEPTED, nMessageID);
        return;
    } else {
        LogPrint(BCLog::COINJOIN, "DSACCEPT -- not compatible with existing transactions!\n");
        PushStatus(peer, STATUS_REJECTED, nMessageID);
        return;
    }
}

void CCoinJoinServer::ProcessDSQUEUE(NodeId from, CDataStream& vRecv)
{
    assert(m_mn_metaman.IsValid());

    CCoinJoinQueue dsq;
    vRecv >> dsq;

    WITH_LOCK(cs_main, m_peer_manager->PeerEraseObjectRequest(from, CInv{MSG_DSQ, dsq.GetHash()}));

    // Validate denomination first
    if (!CoinJoin::IsValidDenomination(dsq.nDenom)) {
        LogPrint(BCLog::COINJOIN, "DSQUEUE -- invalid denomination %d from peer %d\n", dsq.nDenom, from);
        m_peer_manager->PeerMisbehaving(from, 10);
        return;
    }

    if (dsq.masternodeOutpoint.IsNull() && dsq.m_protxHash.IsNull()) {
        m_peer_manager->PeerMisbehaving(from, 100);
        return;
    }

    const auto tip_mn_list = m_dmnman.GetListAtChainTip();
    if (dsq.masternodeOutpoint.IsNull()) {
        if (auto dmn = tip_mn_list.GetValidMN(dsq.m_protxHash)) {
            dsq.masternodeOutpoint = dmn->collateralOutpoint;
        } else {
            m_peer_manager->PeerMisbehaving(from, 10);
            return;
        }
    }

    {
        const auto isDup = m_queueman.TryCheckDuplicate(dsq);
        if (!isDup.has_value()) return;
        if (*isDup) {
            LogPrint(BCLog::COINJOIN, "DSQUEUE -- Peer %d is sending WAY too many dsq messages for a masternode with collateral %s\n", from, dsq.masternodeOutpoint.ToStringShort());
            return;
        }
    }

    LogPrint(BCLog::COINJOIN, "DSQUEUE -- %s new\n", dsq.ToString());

    if (dsq.IsTimeOutOfBounds()) return;

    auto dmn = tip_mn_list.GetValidMNByCollateral(dsq.masternodeOutpoint);
    if (!dmn) return;

    if (dsq.m_protxHash.IsNull()) {
        dsq.m_protxHash = dmn->proTxHash;
    }

    if (!dsq.CheckSignature(dmn->pdmnState->pubKeyOperator.Get())) {
        m_peer_manager->PeerMisbehaving(from, 10);
        return;
    }

    if (!dsq.fReady) {
        //don't allow a few nodes to dominate the queuing process
        if (m_mn_metaman.IsMixingThresholdExceeded(dmn->proTxHash, tip_mn_list.GetCounts().enabled())) {
            LogPrint(BCLog::COINJOIN, "DSQUEUE -- node sending too many dsq messages, masternode=%s\n", dmn->proTxHash.ToString());
            return;
        }
        m_mn_metaman.AllowMixing(dmn->proTxHash);

        LogPrint(BCLog::COINJOIN, "DSQUEUE -- new CoinJoin queue, masternode=%s, queue=%s\n", dmn->proTxHash.ToString(), dsq.ToString());

        if (!m_queueman.TryAddQueue(dsq)) return;
        WITH_LOCK(cs_main, m_peer_manager->PeerForgetObjectRequest(CInv{MSG_DSQ, dsq.GetHash()}));
        m_peer_manager->PeerRelayDSQ(dsq);
    }
}

void CCoinJoinServer::ProcessDSVIN(CNode& peer, CDataStream& vRecv)
{
    //do we have enough users in the current session?
    if (!WITH_LOCK(cs_coinjoin, return IsSessionReady())) {
        LogPrint(BCLog::COINJOIN, "DSVIN -- session not complete!\n");
        PushStatus(peer, STATUS_REJECTED, ERR_SESSION);
        return;
    }

    CCoinJoinEntry entry;
    vRecv >> entry;

    LogPrint(BCLog::COINJOIN, "DSVIN -- txCollateral %s", entry.txCollateral->ToString()); /* Continued */

    PoolMessage nMessageID = MSG_NOERR;

    entry.addr = peer.addr;
    if (AddEntry(entry, nMessageID)) {
        PushStatus(peer, STATUS_ACCEPTED, nMessageID);
        CheckPool();
        LOCK(cs_coinjoin);
        RelayStatus(STATUS_ACCEPTED);
    } else {
        PushStatus(peer, STATUS_REJECTED, nMessageID);
    }
}

void CCoinJoinServer::ProcessDSSIGNFINALTX(CNode& peer, CDataStream& vRecv)
{
    // Only accept signatures while we are actually collecting them, and only
    // from peers that are active participants in this session. Otherwise a
    // stray or unauthenticated peer could abort the session for everyone.
    if (nState != POOL_STATE_SIGNING) {
        LogPrint(BCLog::COINJOIN, "DSSIGNFINALTX -- wrong state, nState=%d, peer=%d\n",
                 nState.load(), peer.GetId());
        PushStatus(peer, STATUS_REJECTED, ERR_SESSION);
        return;
    }
    {
        LOCK(cs_coinjoin);
        const bool is_participant = std::ranges::any_of(
            vecEntries, [&peer](const auto& entry) { return entry.addr == peer.addr; });
        if (!is_participant) {
            LogPrint(BCLog::COINJOIN, "DSSIGNFINALTX -- ignoring message from non-participant peer=%d\n",
                     peer.GetId());
            PushStatus(peer, STATUS_REJECTED, ERR_INVALID_INPUT);
            return;
        }
    }

    const size_t max_txins{CoinJoin::GetMaxPoolInputOutputCount()};
    std::vector<CTxIn> vecTxIn;
    // Reject an over-cap count through this peer-local ERR_MAXIMUM path before a
    // single CTxIn is decoded or allocated. A count above the generic MAX_SIZE cap
    // is malformed and throws from ReadCompactSize, as it does for any other message.
    if (!UnserializeVectorWithMaxSize(vRecv, vecTxIn, max_txins)) {
        LogPrint(BCLog::COINJOIN, "DSSIGNFINALTX -- too many inputs. max=%d, peer=%d\n",
                 static_cast<int>(max_txins), peer.GetId());
        PushStatus(peer, STATUS_REJECTED, ERR_MAXIMUM);
        return;
    }

    LogPrint(BCLog::COINJOIN, "DSSIGNFINALTX -- vecTxIn.size() %s\n", vecTxIn.size());

    int nTxInIndex = 0;
    int nTxInsCount = (int)vecTxIn.size();

    for (const auto& txin : vecTxIn) {
        nTxInIndex++;
        if (!AddScriptSig(txin)) {
            LogPrint(BCLog::COINJOIN, "DSSIGNFINALTX -- AddScriptSig() failed at %d/%d, session: %d\n", nTxInIndex, nTxInsCount, nSessionID);
            LOCK(cs_coinjoin);
            RelayStatus(STATUS_REJECTED);
            return;
        }
        LogPrint(BCLog::COINJOIN, "DSSIGNFINALTX -- AddScriptSig() %d/%d success\n", nTxInIndex, nTxInsCount);
    }
    // all is good
    CheckPool();
}

void CCoinJoinServer::SetNull()
{
    AssertLockHeld(cs_coinjoin);
    // MN side
    m_session_collaterals.Clear();

    CCoinJoinBaseSession::SetNull();
    m_queueman.SetNull();
}

//
// Check the mixing progress and send client updates if a Masternode
//
void CCoinJoinServer::CheckPool()
{
    AssertLockNotHeld(cs_coinjoin);

    // Both the scheduler thread and the message-handling thread get here. Skip the round if the
    // other one is already in it rather than blocking msghand behind its mempool work: whichever
    // thread holds the lock is performing the same check we would.
    TRY_LOCK(cs_check_pool, lock_check_pool);
    if (!lock_check_pool) return;

    // Decide what to do from a single consistent snapshot. Sampling nState, the entry count and
    // the collateral count under separate lock acquisitions let a concurrent SetNull() land
    // between them, so an already-reset session could be read as "0 entries == 0 collaterals"
    // and finalized: an empty final transaction, a dead session put back into SIGNING, and no
    // new session accepted until that timed out.
    enum class Action : uint8_t {
        None,
        Finalize,
        ChargeAndFinalize,
        Commit
    };
    Action action{Action::None};
    int session_id{0};
    {
        LOCK(cs_coinjoin);
        session_id = nSessionID;
        const int entries{GetEntriesCountLocked()};
        if (entries != 0) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CheckPool -- entries count %lu\n", entries);
        }
        if (nState == POOL_STATE_ACCEPTING_ENTRIES) {
            if (static_cast<size_t>(entries) == m_session_collaterals.size()) {
                // We have an entry for each collateral
                action = Action::Finalize;
            } else if (CCoinJoinServer::HasTimedOut() && entries >= CoinJoin::GetMinPoolParticipants()) {
                // We timed out while accepting entries but still have more than the minimum, so
                // punish the misbehaving participants and complete the session without them
                action = Action::ChargeAndFinalize;
            }
        } else if (nState == POOL_STATE_SIGNING && IsSignaturesComplete()) {
            action = Action::Commit;
        }
    }

    switch (action) {
    case Action::None:
        return;
    case Action::ChargeAndFinalize:
    case Action::Finalize:
        CreateFinalTransaction(session_id, action == Action::ChargeAndFinalize);
        return;
    case Action::Commit:
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CheckPool -- SIGNING\n");
        CommitFinalTransaction(session_id);
        return;
    }
}

void CCoinJoinServer::CreateFinalTransaction(int session_id, bool charge_fees)
{
    AssertLockNotHeld(cs_coinjoin);
    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CreateFinalTransaction -- FINALIZE TRANSACTIONS\n");

    CTransactionRef collateral_to_charge;
    {
        LOCK(cs_coinjoin);

        // Finalizing a session that is already gone would put it back into SIGNING and reject every
        // new one until that timed out. Requiring the accepting state also makes selecting an
        // offender and closing entry admission one atomic operation.
        if (nSessionID != session_id || nState != POOL_STATE_ACCEPTING_ENTRIES) {
            LogPrint(BCLog::COINJOIN, /* Continued */
                     "CCoinJoinServer::CreateFinalTransaction -- session %d is gone or no longer accepting entries\n",
                     session_id);
            return;
        }

        if (charge_fees) {
            collateral_to_charge = SelectCollateralToCharge();
        }

        CMutableTransaction txNew;

        // make our new transaction
        for (const auto& entry : vecEntries) {
            for (const auto& txout : entry.vecTxOut) {
                txNew.vout.push_back(txout);
            }
            for (const auto& txdsin : entry.vecTxDSIn) {
                txNew.vin.push_back(txdsin);
            }
        }

        sort(txNew.vin.begin(), txNew.vin.end(), CompareInputBIP69());
        sort(txNew.vout.begin(), txNew.vout.end(), CompareOutputBIP69());

        finalMutableTransaction = txNew;
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CreateFinalTransaction -- finalMutableTransaction=%s", /* Continued */
                 txNew.ToString());

        // request signatures from clients
        SetState(POOL_STATE_SIGNING);
        RelayFinalTransaction(CTransaction(finalMutableTransaction));
    }

    if (collateral_to_charge) {
        ConsumeCollateral(collateral_to_charge);
    }
}

void CCoinJoinServer::CommitFinalTransaction(int session_id)
{
    AssertLockNotHeld(cs_coinjoin);

    CTransactionRef finalTransaction;
    std::vector<CService> participants;
    std::vector<CTransactionRef> collaterals;
    {
        LOCK(cs_coinjoin);
        // Committing a session that is already gone would push a cleared finalMutableTransaction
        // through ATMP and notify the participants of a failure that never happened.
        if (nSessionID != session_id) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CommitFinalTransaction -- session %d is gone, not committing\n",
                     session_id);
            return;
        }
        finalTransaction = MakeTransactionRef(finalMutableTransaction);
        participants.reserve(vecEntries.size());
        std::ranges::transform(vecEntries, std::back_inserter(participants), [](const auto& entry) { return entry.addr; });
        collaterals = m_session_collaterals.txs();
    }
    uint256 hashTx = finalTransaction->GetHash();

    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CommitFinalTransaction -- finalTransaction=%s", /* Continued */
             finalTransaction->ToString());

    {
        // See if the transaction is valid
        TRY_LOCK(::cs_main, lockMain);
        mempool.PrioritiseTransaction(hashTx, 0.1 * COIN);
        if (!lockMain || !ATMPIfSaneFee(m_chainman, finalTransaction)) {
            LogPrint(BCLog::COINJOIN, /* Continued */
                     "CCoinJoinServer::CommitFinalTransaction -- ATMPIfSaneFee() error: Transaction not valid\n");
            // not much we can do in this case, just notify clients
            RelayCompletedTransaction(session_id, participants, ERR_INVALID_TX);
            ResetSigningSessionIfCurrent(session_id);
            return;
        }
    }

    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CommitFinalTransaction -- CREATING DSTX\n");

    // create and sign masternode dstx transaction
    if (!m_dstxman.GetDSTX(hashTx)) {
        CCoinJoinBroadcastTx dstxNew(finalTransaction, m_mn_activeman.GetOutPoint(), m_mn_activeman.GetProTxHash(),
                                     GetAdjustedTime());
        dstxNew.vchSig = m_mn_activeman.SignBasic(dstxNew.GetSignatureHash());
        m_dstxman.AddDSTX(dstxNew);
    }

    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CommitFinalTransaction -- TRANSMITTING DSTX\n");

    CInv inv(MSG_DSTX, hashTx);
    m_peer_manager->PeerRelayInv(inv);

    // Tell the clients it was successful
    RelayCompletedTransaction(session_id, participants, MSG_SUCCESS);

    // Randomly charge clients
    ChargeRandomFees(collaterals);

    // Reset
    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CommitFinalTransaction -- COMPLETED -- RESETTING\n");
    ResetSigningSessionIfCurrent(session_id);
}

//
// Charge clients a fee if they're abusive
//
// Why bother? CoinJoin uses collateral to ensure abuse to the process is kept to a minimum.
// The submission and signing stages are completely separate. In the cases where
// a client submits a transaction then refused to sign, there must be a cost. Otherwise, they
// would be able to do this over and over again and bring the mixing to a halt.
//
// How does this work? Messages to Masternodes come in via NetMsgType::DSVIN, these require a valid collateral
// transaction for the client to be able to enter the pool. This transaction is kept by the Masternode
// until the transaction is either complete or fails.
//
/*
 * Select one offender while cs_coinjoin still binds the state, entries and collaterals to the
 * same session. The caller must close the relevant admission path before releasing the lock and
 * consuming the returned collateral, so a late entry or signature cannot make the snapshot stale.
 */
CTransactionRef CCoinJoinServer::SelectCollateralToCharge() const
{
    AssertLockHeld(cs_coinjoin);

    //we don't need to charge collateral for every offence.
    if (GetRand<int>(/*nMax=*/100) > 33) return {};

    std::vector<CTransactionRef> vecOffendersCollaterals;
    const PoolState state{nState};
    const size_t nSessionCollaterals{m_session_collaterals.size()};

    if (state == POOL_STATE_ACCEPTING_ENTRIES) {
        for (const auto& txCollateral : m_session_collaterals.txs()) {
            bool fFound = std::ranges::any_of(vecEntries, [&txCollateral](const auto& entry) {
                return *entry.txCollateral == *txCollateral;
            });

            // This queue entry didn't send us the promised transaction
            if (!fFound) {
                LogPrint(BCLog::COINJOIN, /* Continued */
                         "CCoinJoinServer::SelectCollateralToCharge -- found uncooperative node (didn't send "
                         "transaction), found offence\n");
                vecOffendersCollaterals.push_back(txCollateral);
            }
        }
    } else if (state == POOL_STATE_SIGNING) {
        // who didn't sign?
        for (const auto& entry : vecEntries) {
            for (const auto& txdsin : entry.vecTxDSIn) {
                if (!txdsin.fHasSig) {
                    LogPrint(BCLog::COINJOIN, /* Continued */
                             "CCoinJoinServer::SelectCollateralToCharge -- found uncooperative node (didn't sign), "
                             "found offence\n");
                    vecOffendersCollaterals.push_back(entry.txCollateral);
                }
            }
        }
    }

    // no offences found
    if (vecOffendersCollaterals.empty()) return {};

    //mostly offending? Charge sometimes
    if (vecOffendersCollaterals.size() >= nSessionCollaterals - 1 && GetRand<int>(/*nMax=*/100) > 33) return {};

    //everyone is an offender? That's not right
    if (vecOffendersCollaterals.size() >= nSessionCollaterals) return {};

    //charge one of the offenders randomly
    Shuffle(vecOffendersCollaterals.begin(), vecOffendersCollaterals.end(), FastRandomContext());

    LogPrint(BCLog::COINJOIN, /* Continued */
             "CCoinJoinServer::SelectCollateralToCharge -- found uncooperative node (didn't %s transaction), charging "
             "fees: %s",
             (state == POOL_STATE_SIGNING) ? "sign" : "send", vecOffendersCollaterals[0]->ToString());
    return vecOffendersCollaterals[0];
}

/*
    Charge the collateral randomly.
    Mixing is completely free, to pay miners we randomly pay the collateral of users.

    Collateral Fee Charges:

    Being that mixing has "no fees" we need to have some kind of cost associated
    with using it to stop abuse. Otherwise, it could serve as an attack vector and
    allow endless transaction that would bloat Dash and make it unusable. To
    stop these kinds of attacks 1 in 10 successful transactions are charged. This
    adds up to a cost of 0.001DRK per transaction on average.
*/
void CCoinJoinServer::ChargeRandomFees(const std::vector<CTransactionRef>& collaterals) const
{
    AssertLockNotHeld(cs_coinjoin);

    for (const auto& txCollateral : collaterals) {
        if (GetRand<int>(/*nMax=*/100) > 10) return;
        LogPrint(BCLog::COINJOIN, /* Continued */
                 "CCoinJoinServer::ChargeRandomFees -- charging random fees, txCollateral=%s", txCollateral->ToString());
        ConsumeCollateral(txCollateral);
    }
}

void CCoinJoinServer::ConsumeCollateral(const CTransactionRef& txref) const
{
    LOCK(::cs_main);
    if (!ATMPIfSaneFee(m_chainman, txref)) {
        LogPrint(BCLog::COINJOIN, "%s -- ATMPIfSaneFee failed\n", __func__);
    } else {
        m_peer_manager->PeerRelayTransaction(txref->GetHash());
        LogPrint(BCLog::COINJOIN, "%s -- Collateral was consumed\n", __func__);
    }
}

bool CCoinJoinServer::HasTimedOut() const
{
    if (nState == POOL_STATE_IDLE) return false;

    int nTimeout = (nState == POOL_STATE_SIGNING) ? COINJOIN_SIGNING_TIMEOUT : COINJOIN_QUEUE_TIMEOUT;

    return GetTime() - nTimeLastSuccessfulStep >= nTimeout;
}

//
// Check for extraneous timeout
//
void CCoinJoinServer::CheckTimeout()
{
    m_queueman.CheckQueue();

    // CheckPool can be finalizing or committing on the message-handling thread. Skipping this tick
    // keeps timeout reset and finalization/commit single-flight without blocking the scheduler.
    TRY_LOCK(cs_check_pool, lock_check_pool);
    if (!lock_check_pool) return;

    CTransactionRef collateral_to_charge;
    {
        LOCK(cs_coinjoin);

        // Too early to do anything. Recheck while holding the lock so selecting an offender and
        // closing the session form one atomic cutoff for late entries and signatures.
        if (!CCoinJoinServer::HasTimedOut()) return;

        // CheckForCompleteQueue() and CheckPool() run before this method on the scheduler thread,
        // but a final collateral, entry, or signature can arrive after their snapshots. The
        // message-handling thread then skips its own CheckPool() if this scheduler tick still holds
        // cs_check_pool. Give a session that can now advance priority over resetting it; the next
        // scheduler tick will perform the transition, finalization, or commit.
        const int entries{GetEntriesCountLocked()};
        const bool can_advance{
            (nState == POOL_STATE_QUEUE && IsSessionReady()) ||
            (nState == POOL_STATE_ACCEPTING_ENTRIES &&
             (static_cast<size_t>(entries) == m_session_collaterals.size() ||
              entries >= CoinJoin::GetMinPoolParticipants())) ||
            (nState == POOL_STATE_SIGNING && IsSignaturesComplete())};
        if (can_advance) return;

        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CheckTimeout -- %s timed out -- resetting\n",
                 (nState == POOL_STATE_SIGNING) ? "Signing" : "Session");
        collateral_to_charge = SelectCollateralToCharge();
        SetNull();
    }

    if (collateral_to_charge) {
        ConsumeCollateral(collateral_to_charge);
    }
}

/*
    Check to see if we're ready for submissions from clients
    After receiving multiple dsa messages, the queue will switch to "accepting entries"
    which is the active state right before merging the transaction
*/
void CCoinJoinServer::CheckForCompleteQueue()
{
    AssertLockNotHeld(cs_coinjoin);

    int session_denom;
    size_t participants;
    {
        LOCK(cs_coinjoin);
        if (nState != POOL_STATE_QUEUE || !IsSessionReady()) return;

        SetState(POOL_STATE_ACCEPTING_ENTRIES);
        session_denom = nSessionDenom;
        participants = m_session_collaterals.size();
    }

    CCoinJoinQueue dsq(session_denom, m_mn_activeman.GetOutPoint(), m_mn_activeman.GetProTxHash(), GetAdjustedTime(), true);
    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CheckForCompleteQueue -- ready queue %s with %d participants\n",
             dsq.ToString(), participants);
    dsq.vchSig = m_mn_activeman.SignBasic(dsq.GetSignatureHash());
    m_peer_manager->PeerRelayDSQ(dsq);
    m_queueman.AddQueue(std::move(dsq));
}

// Check to make sure a given input matches an input in the pool and its scriptSig is valid
bool CCoinJoinServer::IsInputScriptSigValid(const CTxIn& txin) const
{
    AssertLockHeld(cs_coinjoin);
    CMutableTransaction txNew;
    txNew.vin.clear();
    txNew.vout.clear();

    int nTxInIndex = -1;
    CScript sigPubKey = CScript();

    {
        int i = 0;
        for (const auto &entry: vecEntries) {
            for (const auto &txout: entry.vecTxOut) {
                txNew.vout.push_back(txout);
            }
            for (const auto &txdsin: entry.vecTxDSIn) {
                txNew.vin.push_back(txdsin);

                if (txdsin.prevout == txin.prevout) {
                    nTxInIndex = i;
                    sigPubKey = txdsin.prevPubKey;
                }
                i++;
            }
        }
    }
    if (nTxInIndex >= 0) { //might have to do this one input at a time?
        txNew.vin[nTxInIndex].scriptSig = txin.scriptSig;
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::IsInputScriptSigValid -- verifying scriptSig %s\n", ScriptToAsmStr(txin.scriptSig).substr(0, 24));
        // TODO we're using amount=0 here but we should use the correct amount. This works because Dash ignores the amount while signing/verifying (only used in Bitcoin/Segwit)
        if (!VerifyScript(txNew.vin[nTxInIndex].scriptSig, sigPubKey, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, MutableTransactionSignatureChecker(&txNew, nTxInIndex, 0, MissingDataBehavior::ASSERT_FAIL))) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::IsInputScriptSigValid -- VerifyScript() failed on input %d\n", nTxInIndex);
            return false;
        }
    } else {
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::IsInputScriptSigValid -- Failed to find matching input in pool, %s\n", txin.ToString());
        return false;
    }

    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::IsInputScriptSigValid -- Successfully validated input and scriptSig\n");
    return true;
}

//
// Add a client's transaction inputs/outputs to the pool
//
bool CCoinJoinServer::AddEntry(const CCoinJoinEntry& entry, PoolMessage& nMessageIDRet)
{
    AssertLockNotHeld(cs_coinjoin);

    // Remember which session we are admitting this entry to. The validation below releases
    // cs_coinjoin and takes cs_main, so the session can be reset underneath us before we commit.
    int session_id{0};
    int session_denom{0};
    {
        LOCK(cs_coinjoin);
        session_id = nSessionID;
        session_denom = nSessionDenom;

        if (nState != POOL_STATE_ACCEPTING_ENTRIES) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- ERROR: session is not accepting entries!\n", __func__);
            nMessageIDRet = ERR_SESSION;
            return false;
        }

        // Cheap gate before the cs_main work below; the authoritative check is at commit time.
        if (static_cast<size_t>(GetEntriesCountLocked()) >= m_session_collaterals.size()) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- ERROR: entries is full!\n", __func__);
            nMessageIDRet = ERR_ENTRIES_FULL;
            return false;
        }
    }

    if (entry.vecTxDSIn.size() > COINJOIN_ENTRY_MAX_SIZE || entry.vecTxOut.size() > COINJOIN_ENTRY_MAX_SIZE) {
        LogPrint(BCLog::COINJOIN, /* Continued */
                 "CCoinJoinServer::%s -- ERROR: too many inputs or outputs! inputs=%s/%s, outputs=%s/%s\n", __func__,
                 entry.vecTxDSIn.size(), COINJOIN_ENTRY_MAX_SIZE, entry.vecTxOut.size(), COINJOIN_ENTRY_MAX_SIZE);
        nMessageIDRet = ERR_MAXIMUM;

        CTransactionRef txCollateralToConsume;
        {
            LOCK(cs_coinjoin);
            const auto& txs = m_session_collaterals.txs();
            const auto it = std::ranges::find_if(txs, [&entry](const auto& txCollateral) {
                return *entry.txCollateral == *txCollateral;
            });
            if (it != txs.end()) {
                txCollateralToConsume = *it;
            }
        }
        if (txCollateralToConsume) {
            ConsumeCollateral(txCollateralToConsume);
        }
        return false;
    }

    if (!CoinJoin::IsCollateralValid(m_chainman, m_isman, mempool, *entry.txCollateral)) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- ERROR: collateral not valid!\n", __func__);
        nMessageIDRet = ERR_INVALID_COLLATERAL;
        return false;
    }

    std::vector<CTxIn> vin;
    {
        LOCK(cs_coinjoin);
        for (const auto& txin : entry.vecTxDSIn) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- txin=%s\n", __func__, txin.ToString());
            for (const auto& inner_entry : vecEntries) {
                if (std::ranges::any_of(inner_entry.vecTxDSIn,
                                        [&txin](const auto& txdsin) { return txdsin.prevout == txin.prevout; })) {
                    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- ERROR: already have this txin in entries\n",
                             __func__);
                    nMessageIDRet = ERR_ALREADY_HAVE;
                    // Two peers sent the same input? Can't really say who is the malicious one here,
                    // could be that someone is picking someone else's inputs randomly trying to force
                    // collateral consumption. Do not punish.
                    return false;
                }
            }
            vin.emplace_back(txin);
        }
    }

    bool fConsumeCollateral{false};
    if (!IsValidInOuts(m_chainman.ActiveChainstate(), m_isman, mempool, vin, entry.vecTxOut, session_denom,
                       nMessageIDRet, &fConsumeCollateral)) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- ERROR! IsValidInOuts() failed: %s\n", __func__, CoinJoin::GetMessageByID(nMessageIDRet).translated);
        if (fConsumeCollateral) {
            ConsumeCollateral(entry.txCollateral);
        }
        return false;
    }

    int nEntries{0};
    {
        LOCK(cs_coinjoin);

        // IsCollateralValid() and IsValidInOuts() above take cs_main and can block for a long
        // time behind block validation, so a scheduler-thread timeout can reset the session in
        // that window. Committing then would leave an entry of a dead session in vecEntries: the
        // next session inherits it, counts it towards its own participants, and finalizes a
        // transaction containing an input nobody present is going to sign.
        if (nSessionID != session_id || nState != POOL_STATE_ACCEPTING_ENTRIES) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- ERROR: session %d is gone!\n", __func__, session_id);
            nMessageIDRet = ERR_SESSION;
            return false;
        }
        if (static_cast<size_t>(GetEntriesCountLocked()) >= m_session_collaterals.size()) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- ERROR: entries is full!\n", __func__);
            nMessageIDRet = ERR_ENTRIES_FULL;
            return false;
        }

        vecEntries.push_back(entry);
        nEntries = GetEntriesCountLocked();
    }

    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- adding entry %d of %d required\n", __func__, nEntries,
             CoinJoin::GetMaxPoolParticipants());
    nMessageIDRet = MSG_ENTRIES_ADDED;

    return true;
}

bool CCoinJoinServer::AddScriptSig(const CTxIn& txinNew)
{
    AssertLockNotHeld(cs_coinjoin);
    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::AddScriptSig -- scriptSig=%s\n", ScriptToAsmStr(txinNew.scriptSig).substr(0, 24));

    LOCK(cs_coinjoin);
    for (const auto& entry : vecEntries) {
        if (std::ranges::any_of(entry.vecTxDSIn,
                                [&txinNew](const auto& txdsin) { return txdsin.scriptSig == txinNew.scriptSig; })) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::AddScriptSig -- already exists\n");
            return false;
        }
    }

    if (!IsInputScriptSigValid(txinNew)) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::AddScriptSig -- Invalid scriptSig\n");
        return false;
    }

    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::AddScriptSig -- scriptSig=%s new\n", ScriptToAsmStr(txinNew.scriptSig).substr(0, 24));

    for (auto& txin : finalMutableTransaction.vin) {
        if (txin.prevout == txinNew.prevout && txin.nSequence == txinNew.nSequence) {
            txin.scriptSig = txinNew.scriptSig;
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::AddScriptSig -- adding to finalMutableTransaction, scriptSig=%s\n", ScriptToAsmStr(txinNew.scriptSig).substr(0, 24));
        }
    }
    for (auto& entry : vecEntries) {
        if (entry.AddScriptSig(txinNew)) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::AddScriptSig -- adding to entries, scriptSig=%s\n", ScriptToAsmStr(txinNew.scriptSig).substr(0, 24));
            return true;
        }
    }

    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::AddScriptSig -- Couldn't set sig!\n");
    return false;
}

// Check to make sure everything is signed
bool CCoinJoinServer::IsSignaturesComplete() const
{
    AssertLockHeld(cs_coinjoin);

    return std::ranges::all_of(vecEntries, [](const auto& entry) {
        return std::ranges::all_of(entry.vecTxDSIn, [](const auto& txdsin) { return txdsin.fHasSig; });
    });
}

bool CCoinJoinServer::IsAcceptableDSA(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet) const
{
    // is denom even something legit?
    if (!CoinJoin::IsValidDenomination(dsa.nDenom)) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- denom not valid!\n", __func__);
        nMessageIDRet = ERR_DENOM;
        return false;
    }

    // check collateral
    if (!fUnitTest && !CoinJoin::IsCollateralValid(m_chainman, m_isman, mempool, CTransaction(dsa.txCollateral))) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- collateral not valid!\n", __func__);
        nMessageIDRet = ERR_INVALID_COLLATERAL;
        return false;
    }

    return true;
}

bool CCoinJoinServer::CreateNewSession(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet)
{
    if (nSessionID != 0) return false;

    // new session can only be started in idle mode
    if (nState != POOL_STATE_IDLE) {
        nMessageIDRet = ERR_MODE;
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CreateNewSession -- incompatible mode: nState=%d\n", nState);
        return false;
    }

    if (!IsAcceptableDSA(dsa, nMessageIDRet)) {
        return false;
    }

    int nDenom{0};
    size_t nParticipants{0};
    {
        LOCK(cs_coinjoin);

        // A scheduler-thread timeout can reset the session via SetNull() between the checks
        // above and taking cs_coinjoin, so revalidate: the session state and the collateral
        // that opened it have to be committed as one unit.
        if (nSessionID != 0 || nState != POOL_STATE_IDLE) {
            nMessageIDRet = ERR_MODE;
            return false;
        }

        // start new session
        nMessageIDRet = MSG_NOERR;
        nSessionID = GetRand<int>(/*nMax=*/999999) + 1;
        nSessionDenom = dsa.nDenom;

        SetState(POOL_STATE_QUEUE);

        m_session_collaterals.Add(dsa.txCollateral);
        nDenom = nSessionDenom;
        nParticipants = m_session_collaterals.size();
    }

    if (!fUnitTest) {
        //broadcast that I'm accepting entries, only if it's the first entry through
        CCoinJoinQueue dsq(nDenom, m_mn_activeman.GetOutPoint(), m_mn_activeman.GetProTxHash(), GetAdjustedTime(), false);
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::CreateNewSession -- signing and relaying new queue: %s\n", dsq.ToString());
        dsq.vchSig = m_mn_activeman.SignBasic(dsq.GetSignatureHash());
        m_peer_manager->PeerRelayDSQ(dsq);
        m_queueman.AddQueue(std::move(dsq));
    }

    LogPrint(BCLog::COINJOIN, /* Continued */
             "CCoinJoinServer::CreateNewSession -- new session created, nSessionID: %d  nSessionDenom: %d (%s)  "
             "participants: %d  CoinJoin::GetMaxPoolParticipants(): %d\n",
             nSessionID, nSessionDenom, CoinJoin::DenominationToString(nSessionDenom), nParticipants,
             CoinJoin::GetMaxPoolParticipants());

    return true;
}

bool CCoinJoinServer::AddUserToExistingSession(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet)
{
    int session_id;
    int session_denom;
    {
        LOCK(cs_coinjoin);
        if (nSessionID == 0 || nState != POOL_STATE_QUEUE) {
            nMessageIDRet = ERR_MODE;
            return false;
        }
        if (IsSessionReady()) {
            nMessageIDRet = ERR_QUEUE_FULL;
            return false;
        }
        session_id = nSessionID;
        session_denom = nSessionDenom;
    }

    if (!IsAcceptableDSA(dsa, nMessageIDRet)) {
        return false;
    }

    if (dsa.nDenom != session_denom) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::AddUserToExistingSession -- incompatible denom %d (%s) != %d (%s)\n",
                 dsa.nDenom, CoinJoin::DenominationToString(dsa.nDenom), session_denom,
                 CoinJoin::DenominationToString(session_denom));
        nMessageIDRet = ERR_DENOM;
        return false;
    }

    LOCK(cs_coinjoin);

    if (nSessionID != session_id || nSessionDenom != session_denom || nState != POOL_STATE_QUEUE) {
        nMessageIDRet = ERR_MODE;
        return false;
    }
    if (IsSessionReady()) {
        nMessageIDRet = ERR_QUEUE_FULL;
        return false;
    }

    // A resent or replayed dsa must not be counted as a new participant; see SessionCollaterals.
    if (const auto prevout = m_session_collaterals.FindCommittedPrevout(dsa.txCollateral)) {
        LogPrint(BCLog::COINJOIN, /* Continued */
                 "CCoinJoinServer::AddUserToExistingSession -- collateral %s spends prevout %s already committed to "
                 "this session\n",
                 dsa.txCollateral.GetHash().ToString(), prevout->ToStringShort());
        nMessageIDRet = ERR_ALREADY_HAVE;
        return false;
    }

    // count new user as accepted to an existing session

    nMessageIDRet = MSG_NOERR;
    m_session_collaterals.Add(dsa.txCollateral);

    LogPrint(BCLog::COINJOIN, /* Continued */
             "CCoinJoinServer::AddUserToExistingSession -- new user accepted, nSessionID: %d  nSessionDenom: %d (%s)  "
             "participants: %d  CoinJoin::GetMaxPoolParticipants(): %d\n",
             nSessionID, nSessionDenom, CoinJoin::DenominationToString(nSessionDenom), m_session_collaterals.size(),
             CoinJoin::GetMaxPoolParticipants());

    return true;
}

// Returns true if either max size has been reached or if the mix timed out and min size was reached
bool CCoinJoinServer::IsSessionReady() const
{
    AssertLockHeld(cs_coinjoin);

    if (nState == POOL_STATE_QUEUE) {
        if ((int)m_session_collaterals.size() >= CoinJoin::GetMaxPoolParticipants()) {
            return true;
        }
        if (CCoinJoinServer::HasTimedOut() && (int)m_session_collaterals.size() >= CoinJoin::GetMinPoolParticipants()) {
            return true;
        }
    }
    if (nState == POOL_STATE_ACCEPTING_ENTRIES) {
        return true;
    }
    return false;
}

void CCoinJoinServer::RelayFinalTransaction(const CTransaction& txFinal)
{
    AssertLockHeld(cs_coinjoin);
    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- nSessionID: %d  nSessionDenom: %d (%s)\n",
        __func__, nSessionID, nSessionDenom, CoinJoin::DenominationToString(nSessionDenom));

    // final mixing tx with empty signatures should be relayed to mixing participants only
    for (const auto& entry : vecEntries) {
        bool fOk = connman.ForNode(entry.addr, [&txFinal, this](CNode* pnode) {
            CNetMsgMaker msgMaker(pnode->GetCommonVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSFINALTX, nSessionID.load(), txFinal));
            return true;
        });
        if (!fOk) {
            // no such node? maybe this client disconnected or our own connection went down
            RelayStatus(STATUS_REJECTED);
            break;
        }
    }
}

void CCoinJoinServer::PushStatus(CNode& peer, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID) const
{
    CCoinJoinStatusUpdate psssup(nSessionID, nState, 0, nStatusUpdate, nMessageID);
    connman.PushMessage(&peer, CNetMsgMaker(peer.GetCommonVersion()).Make(NetMsgType::DSSTATUSUPDATE, psssup));
}

void CCoinJoinServer::RelayStatus(PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID)
{
    AssertLockHeld(cs_coinjoin);
    unsigned int nDisconnected{};
    // status updates should be relayed to mixing participants only
    for (const auto& entry : vecEntries) {
        // make sure everyone is still connected
        bool fOk = connman.ForNode(entry.addr, [&nStatusUpdate, &nMessageID, this](CNode* pnode) {
            PushStatus(*pnode, nStatusUpdate, nMessageID);
            return true;
        });
        if (!fOk) {
            // no such node? maybe this client disconnected or our own connection went down
            ++nDisconnected;
        }
    }
    if (nDisconnected == 0) return; // all is clear

    // something went wrong
    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- can't continue, %llu client(s) disconnected, nSessionID: %d  nSessionDenom: %d (%s)\n",
        __func__, nDisconnected, nSessionID, nSessionDenom, CoinJoin::DenominationToString(nSessionDenom));

    // notify everyone else that this session should be terminated
    for (const auto& entry : vecEntries) {
        connman.ForNode(entry.addr, [this](CNode* pnode) {
            PushStatus(*pnode, STATUS_REJECTED, MSG_NOERR);
            return true;
        });
    }

    if (nDisconnected == vecEntries.size()) {
        // all clients disconnected, there is probably some issues with our own connection
        // do not charge any fees, just reset the pool
        SetNull();
    }
}

void CCoinJoinServer::RelayCompletedTransaction(int session_id, const std::vector<CService>& participants,
                                                PoolMessage nMessageID)
{
    AssertLockNotHeld(cs_coinjoin);
    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- nSessionID: %d\n", __func__, session_id);

    for (const auto& addr : participants) {
        const bool fOk = connman.ForNode(addr, [&nMessageID, session_id, this](CNode* pnode) {
            CNetMsgMaker msgMaker(pnode->GetCommonVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DSCOMPLETE, session_id, nMessageID));
            return true;
        });
        if (!fOk) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinServer::%s -- participant disconnected before completion\n", __func__);
        }
    }
}

void CCoinJoinServer::ResetSigningSessionIfCurrent(int session_id)
{
    AssertLockNotHeld(cs_coinjoin);
    LOCK(cs_coinjoin);
    if (nSessionID != session_id || nState != POOL_STATE_SIGNING) {
        LogPrint(BCLog::COINJOIN, /* Continued */
                 "CCoinJoinServer::%s -- signing session %d is no longer current, not resetting\n", __func__, session_id);
        return;
    }
    SetNull();
}

void CCoinJoinServer::SetState(PoolState nStateNew)
{
    AssertLockHeld(cs_coinjoin);

    if (nStateNew == POOL_STATE_ERROR) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinServer::SetState -- Can't set state to ERROR as a Masternode. \n");
        return;
    }

    LogPrint(BCLog::COINJOIN, "CCoinJoinServer::SetState -- nState: %d, nStateNew: %d\n", nState, nStateNew);
    nTimeLastSuccessfulStep = GetTime();
    nState = nStateNew;
}

void CCoinJoinServer::Schedule(CScheduler& scheduler)
{
    scheduler.scheduleEvery(
        [this]() -> void {
            if (!m_mn_sync.IsBlockchainSynced()) return;
            if (ShutdownRequested()) return;

            CheckForCompleteQueue();
            CheckPool();
            CheckTimeout();
        },
        std::chrono::seconds{1});
}

void CCoinJoinServer::GetJsonInfo(UniValue& obj) const
{
    obj.clear();
    obj.setObject();
    obj.pushKV("queue_size",    m_queueman.GetQueueSize());
    obj.pushKV("denomination",  ValueFromAmount(CoinJoin::DenominationToAmount(nSessionDenom)));
    obj.pushKV("state",         GetStateString());
    obj.pushKV("entries_count", GetEntriesCount());
}

bool CCoinJoinServer::AlreadyHave(const CInv& inv)
{
    return (inv.type == MSG_DSQ) ? m_queueman.HasQueue(inv.hash) : false;
}

bool CCoinJoinServer::ProcessGetData(CNode& pfrom, const CInv& inv, const CNetMsgMaker& msgMaker)
{
    if (inv.type != MSG_DSQ) return false;

    auto opt_dsq = m_queueman.GetQueueFromHash(inv.hash);
    if (!opt_dsq.has_value()) return false;

    connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::DSQUEUE, *opt_dsq));
    return true;
}
