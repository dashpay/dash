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

#include <map>
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

/** Used to keep track of current status of mixing pool
 */
class CCoinJoinServer : public CCoinJoinBaseSession, public NetHandler
{
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

protected:
    // Session state and entry admission live in the protected section so unit tests can seed
    // and drive them through a test subclass.

    // Mixing uses collateral transactions to trust parties entering the pool
    // to behave honestly. If they don't it takes their money.
    std::vector<CTransactionRef> vecSessionCollaterals;
    // Input prevouts of every transaction in vecSessionCollaterals, so a dsa whose collateral
    // reuses one of them can be rejected without rescanning them all.
    std::unordered_set<COutPoint, SaltedOutpointHasher> setSessionCollateralPrevouts GUARDED_BY(cs_coinjoin);

    // Post-V24: true once a participant has been admitted that declared a promotion/demotion,
    // i.e. the final transaction may come out unbalanced. Latched on admission rather than
    // fixed by the creator's protocol version, so a session only commits to this once someone
    // actually asks for it.
    bool m_fRebalanceSession GUARDED_BY(cs_coinjoin){false};
    // Post-V24: true once a participant below COINJOIN_REBALANCE_VERSION has been admitted.
    // Such a peer cannot validate an unbalanced final transaction, so it must never share a
    // session with a rebalance participant. Mutually exclusive with m_fRebalanceSession.
    bool m_fHasLegacyParticipant GUARDED_BY(cs_coinjoin){false};
    // The mixing direction each accepted participant declared in its dsa, keyed by collateral
    // hash. Tells us which side of the session denomination a participant will occupy before
    // its entry arrives, and entitles it (and only it) to submit an entry of that shape.
    std::map<uint256, CoinJoin::MixShape> m_mapDeclaredShapes GUARDED_BY(cs_coinjoin);

    /// Sides of the session denomination the accepted participants declared they will occupy
    CoinJoin::MixSideCounts GetDeclaredSideCounts() const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    /// Add a clients entry to the pool
    bool AddEntry(const CCoinJoinEntry& entry, PoolMessage& nMessageIDRet) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Record an accepted collateral and index its input prevouts
    void CommitSessionCollateral(const CMutableTransaction& txCollateral) EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);
    /// Build and relay the final transaction if the live session is still eligible
    void CreateFinalTransaction(int session_id) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

private:
    bool fUnitTest;

    /// Add signature to a txin
    bool AddScriptSig(const CTxIn& txin) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    /// Charge fees to bad actors (Charge clients a fee if they're abusive)
    void ChargeFees() const EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Rarely charge fees to pay miners
    void ChargeRandomFees() const EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Consume collateral in cases when peer misbehaved. Takes cs_main, which this class never
    /// takes under cs_coinjoin.
    void ConsumeCollateral(const CTransactionRef& txref) const EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Consume collateral, but only while session_id is still the live session holding it
    void ConsumeCollateralIfCurrentSession(int session_id, const CTransactionRef& txref) const
        EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    /// Is session_id still the session we are accepting entries for?
    bool IsCurrentSession(int session_id) const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    //! One consistent view of the session, for decisions that read several pieces of its state.
    //! Sampling them one at a time lets the message-handling thread commit an entry in between,
    //! producing a mix of old and new values that describes no state the session was ever in.
    struct PoolSnapshot {
        int session_id{0};
        PoolState state{POOL_STATE_IDLE};
        size_t entries{0};
        size_t collaterals{0};
        CoinJoin::MixSideCounts sides;
    };
    PoolSnapshot GetPoolSnapshot() const EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Is txref one of the collaterals accepted into the current session?
    bool HasSessionCollateral(const CTransactionRef& txref) const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    /// Check for process
    void CheckPool();

    void CommitFinalTransaction() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    /// Is this nDenom and txCollateral acceptable?
    bool IsAcceptableDSA(const CCoinJoinAccept& dsa, PoolMessage& nMessageIDRet) const;
    bool CreateNewSession(const CCoinJoinAccept& dsa, int nPeerVersion, PoolMessage& nMessageIDRet) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    bool AddUserToExistingSession(const CCoinJoinAccept& dsa, int nPeerVersion, PoolMessage& nMessageIDRet) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Do we have enough users to take entries?
    bool IsSessionReady() const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    /// Check that all inputs are signed. (Are all inputs signed?)
    bool IsSignaturesComplete() const EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    /// Check to make sure a given input matches an input in the pool and its scriptSig is valid
    bool IsInputScriptSigValid(const CTxIn& txin) const EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

    // Set the 'state' value, with some logging and capturing when the state changed
    void SetState(PoolState nStateNew);

    /// Relay mixing Messages
    void RelayFinalTransaction(const CTransaction& txFinal) EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);
    void PushStatus(CNode& peer, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID) const;
    void RelayStatus(PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID = MSG_NOERR) EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);
    void RelayCompletedTransaction(PoolMessage nMessageID) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    void ProcessDSACCEPT(CNode& peer, CDataStream& vRecv) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    void ProcessDSQUEUE(NodeId from, CDataStream& vRecv);
    void ProcessDSVIN(CNode& peer, CDataStream& vRecv) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);
    void ProcessDSSIGNFINALTX(CNode& peer, CDataStream& vRecv) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

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

    void ProcessMessage(CNode& pfrom, const std::string& msg_type, CDataStream& vRecv) override;
    bool ProcessGetData(CNode& pfrom, const CInv& inv, const CNetMsgMaker& msgMaker) override;
    bool AlreadyHave(const CInv& inv) override;
    void Schedule(CScheduler& scheduler) override;

    bool HasTimedOut() const;
    void CheckTimeout();
    void CheckForCompleteQueue();

    void GetJsonInfo(UniValue& obj) const;
};

#endif // BITCOIN_COINJOIN_SERVER_H
