// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINJOIN_SERVER_H
#define BITCOIN_COINJOIN_SERVER_H

#include <coinjoin/coinjoin.h>

#include <net_processing.h>
#include <net_types.h>
#include <protocol.h>
#include <util/hasher.h>

#include <optional>
#include <unordered_set>

class CActiveMasternodeManager;
class CConnman;
class CDataStream;
class CDeterministicMNManager;
class CDSTXManager;
class ChainstateManager;
class CMasternodeMetaMan;
class CNode;
class CTxMemPool;

class UniValue;
namespace coinjoin_inouts_tests {
class TestableCoinJoinServer;
}

/** Used to keep track of current status of mixing pool
 */
class CCoinJoinServer : public CCoinJoinBaseSession, public NetHandler
{
    friend class coinjoin_inouts_tests::TestableCoinJoinServer;

private:
    CoinJoinQueueManager m_queueman;

    ChainstateManager& m_chainman;
    CConnman& connman;
    CDeterministicMNManager& m_dmnman;
    CDSTXManager& m_dstxman;
    CMasternodeMetaMan& m_mn_metaman;
    CTxMemPool& mempool;
    const CActiveMasternodeManager& m_mn_activeman;
    const CMasternodeSync& m_mn_sync;
    const llmq::CInstantSendManager& m_isman;

    /// The collateral transactions of every peer admitted to the current session.
    ///
    /// Mixing uses collateral transactions to trust parties entering the pool to behave
    /// honestly. If they don't it takes their money.
    ///
    /// Session collaterals are only ever test-accepted, never added to the mempool, so nothing
    /// pins their identity: the same UTXO can be re-signed into arbitrarily many distinct txids.
    /// Matching on input prevouts is what makes a resent or replayed dsa recognisable as the
    /// same participant.
    class SessionCollaterals
    {
    public:
        void Add(const CMutableTransaction& txCollateral)
        {
            m_txs.push_back(MakeTransactionRef(txCollateral));
            for (const auto& txin : txCollateral.vin) {
                m_prevouts.insert(txin.prevout);
            }
        }
        void Clear()
        {
            m_txs.clear();
            m_prevouts.clear();
        }
        //! The first input of txCollateral that an already admitted collateral also spends, if any.
        std::optional<COutPoint> FindCommittedPrevout(const CMutableTransaction& txCollateral) const
        {
            for (const auto& txin : txCollateral.vin) {
                if (m_prevouts.contains(txin.prevout)) return txin.prevout;
            }
            return std::nullopt;
        }
        const std::vector<CTransactionRef>& txs() const { return m_txs; }
        size_t size() const { return m_txs.size(); }
        bool empty() const { return m_txs.empty(); }

    private:
        std::vector<CTransactionRef> m_txs;
        std::unordered_set<COutPoint, SaltedOutpointHasher> m_prevouts;
    };
    SessionCollaterals m_session_collaterals GUARDED_BY(cs_coinjoin);

    bool fUnitTest;

    /// Serializes CheckPool() against itself and against CheckTimeout(). CheckPool() runs both on
    /// the scheduler thread and on the message-handling thread, and its finalize and commit steps
    /// have to be single-shot: relaying DSFINALTX twice makes every client sign twice, and the
    /// duplicate signatures then abort the session for all of them. CheckTimeout() uses the same
    /// guard so it cannot reset a session during finalization or commit. Production paths always
    /// acquire it with TRY_LOCK, so a contended caller skips the round rather than blocking msghand.
    Mutex cs_check_pool;

    /// Add a clients entry to the pool
    bool AddEntry(const CCoinJoinEntry& entry, PoolMessage& nMessageIDRet) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Add signature to a txin
    bool AddScriptSig(const CTxIn& txin) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    /// Choose one bad actor whose collateral should be consumed, if any.
    CTransactionRef SelectCollateralToCharge() const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);
    /// Rarely charge fees to pay miners
    void ChargeRandomFees() const EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Consume collateral in cases when peer misbehaved
    void ConsumeCollateral(const CTransactionRef& txref) const;

    /// Check for process
    void CheckPool() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin, !cs_check_pool);

    void CreateFinalTransaction(int session_id, bool charge_fees) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    void CommitFinalTransaction(int session_id) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    /// Is this nDenom and txCollateral acceptable?
    bool IsAcceptableDSA(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet) const;
    bool CreateNewSession(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    bool AddUserToExistingSession(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Do we have enough users to take entries?
    bool IsSessionReady() const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    /// Check that all inputs are signed. (Are all inputs signed?)
    bool IsSignaturesComplete() const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);
    /// Check to make sure a given input matches an input in the pool and its scriptSig is valid
    bool IsInputScriptSigValid(const CTxIn& txin) const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    // Set the 'state' value, with some logging and capturing when the state changed.
    // Requires cs_coinjoin so that a transition and the session data it describes are always
    // observed together: code that revalidates nState under the lock must not have it changed
    // out from under it by a concurrent transition.
    void SetState(PoolState nStateNew) EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    /// Relay mixing Messages
    void RelayFinalTransaction(const CTransaction& txFinal) EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);
    void PushStatus(CNode& peer, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID) const;
    void RelayStatus(PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID = MSG_NOERR) EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);
    void RelayCompletedTransaction(PoolMessage nMessageID) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    void ProcessDSACCEPT(CNode& peer, CDataStream& vRecv) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    void ProcessDSQUEUE(NodeId from, CDataStream& vRecv);
    void ProcessDSVIN(CNode& peer, CDataStream& vRecv) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin, !cs_check_pool);
    void ProcessDSSIGNFINALTX(CNode& peer, CDataStream& vRecv) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin, !cs_check_pool);

    void SetNull() override EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

public:
    CCoinJoinServer() = delete;
    CCoinJoinServer(const CCoinJoinServer&) = delete;
    CCoinJoinServer& operator=(const CCoinJoinServer&) = delete;
    explicit CCoinJoinServer(PeerManagerInternal* peer_manager, ChainstateManager& chainman, CConnman& _connman,
                             CDeterministicMNManager& dmnman, CDSTXManager& dstxman, CMasternodeMetaMan& mn_metaman,
                             CTxMemPool& mempool, const CActiveMasternodeManager& mn_activeman,
                             const CMasternodeSync& mn_sync, const llmq::CInstantSendManager& isman);
    ~CCoinJoinServer() override;

    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override
        EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin, !cs_check_pool);
    bool ProcessGetData(CNode& pfrom, const CInv& inv, const CNetMsgMaker& msgMaker) override;
    bool AlreadyHave(const CInv& inv) override;
    void Schedule(CScheduler& scheduler) override;

    bool HasTimedOut() const;
    void CheckTimeout() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin, !cs_check_pool);
    void CheckForCompleteQueue() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    void GetJsonInfo(UniValue& obj) const;
};

#endif // BITCOIN_COINJOIN_SERVER_H
