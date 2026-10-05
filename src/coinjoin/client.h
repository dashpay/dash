// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINJOIN_CLIENT_H
#define BITCOIN_COINJOIN_CLIENT_H

#include <coinjoin/coinjoin.h>
#include <coinjoin/util.h>
#include <evo/types.h>
#include <interfaces/coinjoin.h>
#include <util/translation.h>

#include <deque>
#include <map>
#include <memory>
#include <ranges>
#include <utility>

namespace wallet {
class WalletBatch;
} // namespace wallet

class CCoinJoinClientManager;
class CConnman;
class CDeterministicMNManager;
class ChainstateManager;
class CMasternodeMetaMan;
class CMasternodeSync;
class CNode;
class CTxMemPool;

class UniValue;

class CPendingDsaRequest
{
private:
    static constexpr int TIMEOUT = 15;

    uint256 proTxHash;
    CCoinJoinAccept dsa;
    int64_t nTimeCreated{0};

public:
    CPendingDsaRequest() = default;

    CPendingDsaRequest(uint256 proTxHash_, CCoinJoinAccept dsa_) :
        proTxHash(std::move(proTxHash_)),
        dsa(std::move(dsa_)),
        nTimeCreated(GetTime())
    {
    }

    [[nodiscard]] uint256 GetProTxHash() const { return proTxHash; }
    [[nodiscard]] const CCoinJoinAccept& GetDSA() const { return dsa; }
    [[nodiscard]] bool IsExpired() const { return GetTime() - nTimeCreated > TIMEOUT; }

    friend bool operator==(const CPendingDsaRequest& a, const CPendingDsaRequest& b)
    {
        return a.proTxHash == b.proTxHash && a.dsa == b.dsa;
    }
    friend bool operator!=(const CPendingDsaRequest& a, const CPendingDsaRequest& b)
    {
        return !(a == b);
    }
    explicit operator bool() const
    {
        return *this != CPendingDsaRequest();
    }
};

class CCoinJoinClientSession : public CCoinJoinBaseSession
{
private:
    const std::shared_ptr<wallet::CWallet> m_wallet;
    CCoinJoinClientManager& m_clientman;
    CDeterministicMNManager& m_dmnman;
    CMasternodeMetaMan& m_mn_metaman;
    const CMasternodeSync& m_mn_sync;
    const llmq::CInstantSendManager& m_isman;
    std::vector<COutPoint> vecOutPointLocked;

    bilingual_str strLastMessage;
    bilingual_str strAutoDenomResult;

    CDeterministicMNCPtr mixingMasternode;
    CMutableTransaction txMyCollateral; // client side collateral
    CPendingDsaRequest pendingDsaRequest;

    CKeyHolderStorage keyHolderStorage; // storage for keys used in PrepareDenominate

    // Post-V24: Promotion/demotion session state
    bool m_fPromotion{false};  // True if this session is promoting smaller -> larger denom
    bool m_fDemotion{false};   // True if this session is demoting larger -> smaller denom
    std::vector<COutPoint> m_vecRebalanceInputs;  // Selected inputs for promotion/demotion rebalancing

    /// Create denominations
    bool CreateDenominated(CAmount nBalanceToDenominate);
    bool CreateDenominated(CAmount nBalanceToDenominate, const wallet::CompactTallyItem& tallyItem, bool fCreateMixingCollaterals)
        EXCLUSIVE_LOCKS_REQUIRED(m_wallet->cs_wallet);

    /// Split up large inputs or make fee sized inputs
    bool MakeCollateralAmounts();
    bool MakeCollateralAmounts(const wallet::CompactTallyItem& tallyItem, bool fTryDenominated)
        EXCLUSIVE_LOCKS_REQUIRED(m_wallet->cs_wallet);

    bool CreateCollateralTransaction(CMutableTransaction& txCollateral, std::string& strReason)
        EXCLUSIVE_LOCKS_REQUIRED(m_wallet->cs_wallet);

    bool JoinExistingQueue(CAmount nBalanceNeedsAnonymized, CConnman& connman,
                           int nTargetDenom = 0, bool fPromotion = false, bool fDemotion = false);
    bool StartNewQueue(CAmount nBalanceNeedsAnonymized, CConnman& connman);
    bool StartNewQueue(CAmount nBalanceNeedsAnonymized, CConnman& connman,
                       int nTargetDenom, bool fPromotion, bool fDemotion);
    CDeterministicMNCPtr GetRandomNotUsedMasternode();

    /// Post-V24: select and lock inputs for a promotion/demotion session. Locked outpoints are
    /// recorded in m_vecRebalanceInputs and vecOutPointLocked so UnlockCoins() releases them on
    /// any failure path.
    bool SelectRebalanceInputs(int nTargetDenom, bool fPromotion, std::vector<CTxDSIn>& vecTxDSInRet);
    /// Post-V24: unlock and forget the inputs selected by SelectRebalanceInputs (session setup failed)
    void UnlockRebalanceInputs();

    /// step 0: select denominated inputs and txouts
    bool SelectDenominate(std::string& strErrorRet, std::vector<CTxDSIn>& vecTxDSInRet);
    /// step 1: prepare denominated inputs and outputs
    bool PrepareDenominate(int nMinRounds, int nMaxRounds, std::string& strErrorRet, const std::vector<CTxDSIn>& vecTxDSIn,
                           std::vector<std::pair<CTxDSIn, CTxOut>>& vecPSInOutPairsRet, bool fDryRun = false)
        EXCLUSIVE_LOCKS_REQUIRED(m_wallet->cs_wallet);

    /// Post-V24: prepare promotion entry (10 inputs of smaller denom -> 1 output of larger denom)
    bool PreparePromotionEntry(std::string& strErrorRet, std::vector<std::pair<CTxDSIn, CTxOut>>& vecPSInOutPairsRet)
        EXCLUSIVE_LOCKS_REQUIRED(m_wallet->cs_wallet);

    /// Post-V24: prepare demotion entry (1 input of larger denom -> 10 outputs of smaller denom)
    bool PrepareDemotionEntry(std::string& strErrorRet, std::vector<std::pair<CTxDSIn, CTxOut>>& vecPSInOutPairsRet)
        EXCLUSIVE_LOCKS_REQUIRED(m_wallet->cs_wallet);

    /// step 2: send denominated inputs and outputs prepared in step 1
    bool SendDenominate(const std::vector<std::pair<CTxDSIn, CTxOut> >& vecPSInOutPairsIn, CConnman& connman) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    /// Process Masternode updates about the progress of mixing
    void ProcessPoolStateUpdate(CCoinJoinStatusUpdate psssup);
    // Set the 'state' value, with some logging and capturing when the state changed
    void SetState(PoolState nStateNew);

    void CompletedTransaction(PoolMessage nMessageID);

    /// As a client, check and sign the final transaction
    bool SignFinalTransaction(CNode& peer, Chainstate& active_chainstate, CConnman& connman, const CTxMemPool& mempool, const CTransaction& finalTransactionNew) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    void RelayIn(const CCoinJoinEntry& entry, CConnman& connman) const;

    void SetNull() override EXCLUSIVE_LOCKS_REQUIRED(cs_coinjoin);

public:
    explicit CCoinJoinClientSession(const std::shared_ptr<wallet::CWallet>& wallet, CCoinJoinClientManager& clientman,
                                    CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
                                    const CMasternodeSync& mn_sync, const llmq::CInstantSendManager& isman);

    void ProcessMessage(CNode& peer, Chainstate& active_chainstate, CConnman& connman, const CTxMemPool& mempool, std::string_view msg_type, CDataStream& vRecv);

    void UnlockCoins();

    void ResetPool() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    bilingual_str GetStatus(bool fWaitForBlock) const;

    bool GetMixingMasternodeInfo(CDeterministicMNCPtr& ret) const;

    /// Passively run mixing in the background according to the configuration in settings
    bool DoAutomaticDenominating(ChainstateManager& chainman, CConnman& connman, const CTxMemPool& mempool,
                                 bool fDryRun = false) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin);

    /// As a client, submit part of a future mixing transaction to a Masternode to start the process
    bool SubmitDenominate(CConnman& connman);

    bool ProcessPendingDsaRequest(CConnman& connman);

    bool CheckTimeout();

    void GetJsonInfo(UniValue& obj) const;
};

/** Used to keep track of current status of mixing pool
 */
class CCoinJoinClientManager : public interfaces::CoinJoin::Client
{
private:
    const std::shared_ptr<wallet::CWallet> m_wallet;
    CDeterministicMNManager& m_dmnman;
    CMasternodeMetaMan& m_mn_metaman;
    const CMasternodeSync& m_mn_sync;
    const llmq::CInstantSendManager& m_isman;
    //! Non-owning pointer; null when relay_txes is disabled (no queue processing).
    CoinJoinQueueManager* const m_queueman;

    mutable Mutex cs_deqsessions;
    // TODO: or map<denom, CCoinJoinClientSession> ??
    std::deque<CCoinJoinClientSession> deqSessions GUARDED_BY(cs_deqsessions);

    int nCachedLastSuccessBlock{0};
    // how many blocks to wait for after one successful mixing tx in non-multisession mode
    static constexpr int nMinBlocksToWait{1};

    mutable Mutex cs_pending_obs;
    //! Inputs of successfully completed mixing sessions which must stay locked until
    //! the wallet observes the finalized mixing transaction spending them. Without this
    //! a client which processes DSCOMPLETE(MSG_SUCCESS) before the (trickle-relayed)
    //! finalized DSTX arrives could select the very same inputs for another session
    //! (esp. with -coinjoinmultisession) and double-spend them.
    //! Maps outpoint to the time it was added. Mirrored to the wallet database so that
    //! both the locks and their grace period survive a restart, and so that these are
    //! never confused with locks the user set themselves via `lockunspent`.
    std::map<COutPoint, int64_t> m_pending_obs GUARDED_BY(cs_pending_obs);
    bool m_pending_obs_loaded GUARDED_BY(cs_pending_obs){false};
    //! Whether the failure to read the record has been reported already, the read is
    //! retried for as long as this node runs and a corrupt record never becomes readable
    bool m_pending_obs_load_failed GUARDED_BY(cs_pending_obs){false};

    /// Populate m_pending_obs from the wallet database, once per run
    void LoadPendingObservations(wallet::WalletBatch& batch) EXCLUSIVE_LOCKS_REQUIRED(cs_pending_obs);

    // Keep track of current block height
    int nCachedBlockHeight{0};

    bool fCreateAutoBackups{true}; // builtin support for automatic backups

    bool WaitForAnotherBlock() const;

    // Make sure we have enough keys since last backup
    bool CheckAutomaticBackup();

public:
    CCoinJoinClientManager() = delete;
    CCoinJoinClientManager(const CCoinJoinClientManager&) = delete;
    CCoinJoinClientManager& operator=(const CCoinJoinClientManager&) = delete;
    explicit CCoinJoinClientManager(const std::shared_ptr<wallet::CWallet>& wallet, CDeterministicMNManager& dmnman,
                                    CMasternodeMetaMan& mn_metaman, const CMasternodeSync& mn_sync,
                                    const llmq::CInstantSendManager& isman, CoinJoinQueueManager* queueman);
    ~CCoinJoinClientManager() override;

    void ProcessMessage(CNode& peer, Chainstate& active_chainstate, CConnman& connman, const CTxMemPool& mempool, std::string_view msg_type, CDataStream& vRecv) EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);

    bool GetMixingMasternodesInfo(std::vector<CDeterministicMNCPtr>& vecDmnsRet) const EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);

    /// Passively run mixing in the background according to the configuration in settings
    bool DoAutomaticDenominating(ChainstateManager& chainman, CConnman& connman, const CTxMemPool& mempool,
                                 bool fDryRun = false) EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);

    bool TrySubmitDenominate(const uint256& proTxHash, CConnman& connman) EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);
    bool MarkAlreadyJoinedQueueAsTried(CCoinJoinQueue& dsq) const EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);
    bool GetQueueItemAndTry(CCoinJoinQueue& dsq, int nDenomFilter = 0) const;

    void CheckTimeout() EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);

    void ProcessPendingDsaRequest(CConnman& connman) EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);

    void UpdatedSuccessBlock();

    /// Keep the given successfully mixed inputs locked (persistently) until the wallet
    /// observes a transaction spending them
    void AddPendingObservation(const std::vector<COutPoint>& outpoints) EXCLUSIVE_LOCKS_REQUIRED(!cs_pending_obs);
    /// Release pending inputs whose spend has been observed by the wallet
    void CheckPendingObservations(const CTxMemPool& mempool) EXCLUSIVE_LOCKS_REQUIRED(!cs_pending_obs);
    bool IsPendingObservation(const COutPoint& outpoint) const EXCLUSIVE_LOCKS_REQUIRED(!cs_pending_obs);
    size_t GetPendingObservationCount() const EXCLUSIVE_LOCKS_REQUIRED(!cs_pending_obs);

    void UpdatedBlockTip(const CBlockIndex* pindex);

    void DoMaintenance(ChainstateManager& chainman, CConnman& connman, const CTxMemPool& mempool)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);

    // interfaces::CoinJoin::Client overrides
    void disableAutobackups() override { fCreateAutoBackups = false; }
    void resetPool() override EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);
    UniValue getJsonInfo() const override EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions, !cs_pending_obs);
    std::vector<std::string> getSessionStatuses() const override EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);
    std::string getSessionDenoms() const override EXCLUSIVE_LOCKS_REQUIRED(!cs_deqsessions);
    bool isMixing() const override;
    bool startMixing() override;
    void stopMixing() override;

    /**
     * Post-V24: Check if we should promote smaller denominations into larger ones
     * @param nSmallerDenom The smaller denomination to promote from
     * @param nLargerDenom The larger denomination to promote into
     * @param counts Wallet denomination counts, e.g. from CWallet::GetDenominationCounts()
     * @return true if promotion is recommended
     */
    static bool ShouldPromote(int nSmallerDenom, int nLargerDenom, const wallet::CoinJoinDenomCounts& counts);

    /**
     * Post-V24: Check if we should demote larger denominations into smaller ones
     * @param nLargerDenom The larger denomination to demote from
     * @param nSmallerDenom The smaller denomination to demote into
     * @param counts Wallet denomination counts, e.g. from CWallet::GetDenominationCounts()
     * @return true if demotion is recommended
     */
    static bool ShouldDemote(int nLargerDenom, int nSmallerDenom, const wallet::CoinJoinDenomCounts& counts);
};

#endif // BITCOIN_COINJOIN_CLIENT_H
