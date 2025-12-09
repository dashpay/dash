// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coinjoin/coinjoin.h>

#include <bls/bls.h>
#include <chainlock/chainlock.h>
#include <instantsend/instantsend.h>
#include <util/helpers.h>

#include <chain.h>
#include <chainparams.h>
#include <deploymentstatus.h>
#include <txmempool.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/translation.h>
#include <validation.h>
#include <tinyformat.h>

#include <ranges>
#include <string>

constexpr static CAmount DEFAULT_MAX_RAW_TX_FEE{COIN / 10};

bool CCoinJoinEntry::AddScriptSig(const CTxIn& txin)
{
    for (auto& txdsin : vecTxDSIn) {
        if (txdsin.prevout == txin.prevout && txdsin.nSequence == txin.nSequence) {
            if (txdsin.fHasSig) return false;

            txdsin.scriptSig = txin.scriptSig;
            txdsin.fHasSig = true;

            return true;
        }
    }

    return false;
}

uint256 CCoinJoinQueue::GetSignatureHash() const
{
    return SerializeHash(*this, SER_GETHASH, PROTOCOL_VERSION);
}
uint256 CCoinJoinQueue::GetHash() const { return SerializeHash(*this, SER_NETWORK, PROTOCOL_VERSION); }

bool CCoinJoinQueue::CheckSignature(const CBLSPublicKey& blsPubKey) const
{
    if (!CBLSSignature(Span{vchSig}, false).VerifyInsecure(blsPubKey, GetSignatureHash(), false)) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinQueue::CheckSignature -- VerifyInsecure() failed\n");
        return false;
    }

    return true;
}

bool CCoinJoinQueue::IsTimeOutOfBounds(int64_t current_time) const
{
    return current_time - nTime > COINJOIN_QUEUE_TIMEOUT ||
           nTime - current_time > COINJOIN_QUEUE_TIMEOUT;
}

[[nodiscard]] std::string CCoinJoinQueue::ToString() const
{
    return strprintf("nDenom=%d, nTime=%lld, fReady=%s, fTried=%s, masternode=%s",
        nDenom, nTime, util::to_string(fReady), util::to_string(fTried), masternodeOutpoint.ToStringShort());
}

uint256 CCoinJoinBroadcastTx::GetSignatureHash() const
{
    return SerializeHash(*this, SER_GETHASH, PROTOCOL_VERSION);
}

bool CCoinJoinBroadcastTx::CheckSignature(const CBLSPublicKey& blsPubKey) const
{
    if (!CBLSSignature(Span{vchSig}, false).VerifyInsecure(blsPubKey, GetSignatureHash(), false)) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinBroadcastTx::CheckSignature -- VerifyInsecure() failed\n");
        return false;
    }

    return true;
}

bool CCoinJoinBroadcastTx::IsExpired(const CBlockIndex* pindex, const llmq::CChainLocksHandler& clhandler) const
{
    // expire confirmed DSTXes after ~1h since confirmation or chainlocked confirmation
    if (!nConfirmedHeight.has_value() || pindex->nHeight < *nConfirmedHeight) return false; // not mined yet
    if (pindex->nHeight - *nConfirmedHeight > 24) return true; // mined more than an hour ago
    return clhandler.HasChainLock(pindex->nHeight, *pindex->phashBlock);
}

bool CCoinJoinBroadcastTx::IsValidStructure(const CBlockIndex* pindex) const
{
    // some trivial checks only
    if (masternodeOutpoint.IsNull() && m_protxHash.IsNull()) {
        return false;
    }

    const bool fV24Active = pindex && DeploymentActiveAt(*pindex, Params().GetConsensus(), Consensus::DEPLOYMENT_V24);

    // Pre-V24: require balanced input/output counts (1:1 mixing only)
    // Post-V24: allow unbalanced counts (promotion/demotion)
    if (!fV24Active && tx->vin.size() != tx->vout.size()) {
        return false;
    }

    if (tx->vin.size() < size_t(CoinJoin::GetMinPoolParticipants())) {
        return false;
    }

    // Post-V24: allow up to 200 inputs (20 participants * 10 inputs for promotions)
    // Pre-V24: max 180 inputs (20 participants * 9 entries)
    const size_t nMaxInputs = fV24Active
        ? CoinJoin::GetMaxPoolParticipants() * CoinJoin::PROMOTION_RATIO
        : CoinJoin::GetMaxPoolParticipants() * COINJOIN_ENTRY_MAX_SIZE;

    if (tx->vin.size() > nMaxInputs) {
        return false;
    }

    if (!std::ranges::all_of(tx->vout, [](const auto& txOut) {
        return CoinJoin::IsDenominatedAmount(txOut.nValue) && txOut.scriptPubKey.IsPayToPublicKeyHash();
    })) {
        return false;
    }

    // Note: For post-V24 unbalanced transactions (promotion/demotion),
    // value sum validation (inputs == outputs) requires UTXO access and
    // is performed in IsValidInOuts() when the transaction is processed.

    return true;
}

void CCoinJoinBaseSession::SetNull()
{
    // Both sides
    AssertLockHeld(cs_coinjoin);
    nState = POOL_STATE_IDLE;
    nSessionID = 0;
    nSessionDenom = 0;
    vecEntries.clear();
    finalMutableTransaction.vin.clear();
    finalMutableTransaction.vout.clear();
    nTimeLastSuccessfulStep = GetTime();
}

CCoinJoinBaseManager::CCoinJoinBaseManager() = default;

CCoinJoinBaseManager::~CCoinJoinBaseManager() = default;

void CCoinJoinBaseManager::SetNull()
{
    LOCK(cs_vecqueue);
    vecCoinJoinQueue.clear();
}

void CCoinJoinBaseManager::CheckQueue()
{
    TRY_LOCK(cs_vecqueue, lockDS);
    if (!lockDS) return; // it's ok to fail here, we run this quite frequently

    // check mixing queue objects for timeouts
    auto it = vecCoinJoinQueue.begin();
    while (it != vecCoinJoinQueue.end()) {
        if (it->IsTimeOutOfBounds()) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinBaseManager::%s -- Removing a queue (%s)\n", __func__, it->ToString());
            it = vecCoinJoinQueue.erase(it);
        } else {
            ++it;
        }
    }
}

bool CCoinJoinBaseManager::GetQueueItemAndTry(CCoinJoinQueue& dsqRet)
{
    TRY_LOCK(cs_vecqueue, lockDS);
    if (!lockDS) return false; // it's ok to fail here, we run this quite frequently

    for (auto& dsq : vecCoinJoinQueue) {
        // only try each queue once
        if (dsq.fTried || dsq.IsTimeOutOfBounds()) continue;
        dsq.fTried = true;
        dsqRet = dsq;
        return true;
    }

    return false;
}

std::string CCoinJoinBaseSession::GetStateString() const
{
    switch (nState) {
    case POOL_STATE_IDLE:
        return "IDLE";
    case POOL_STATE_QUEUE:
        return "QUEUE";
    case POOL_STATE_ACCEPTING_ENTRIES:
        return "ACCEPTING_ENTRIES";
    case POOL_STATE_SIGNING:
        return "SIGNING";
    case POOL_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

bool CCoinJoinBaseSession::IsValidInOuts(CChainState& active_chainstate, const llmq::CInstantSendManager& isman,
                                         const CTxMemPool& mempool, const std::vector<CTxIn>& vin,
                                         const std::vector<CTxOut>& vout, PoolMessage& nMessageIDRet,
                                         bool* fConsumeCollateralRet) const
{
    std::set<CScript> setScripPubKeys;
    nMessageIDRet = MSG_NOERR;
    if (fConsumeCollateralRet) *fConsumeCollateralRet = false;

    // Check if V24 is active for promotion/demotion support
    bool fV24Active{false};
    {
        LOCK(::cs_main);
        const CBlockIndex* pindex = active_chainstate.m_chain.Tip();
        fV24Active = pindex && DeploymentActiveAt(*pindex, Params().GetConsensus(), Consensus::DEPLOYMENT_V24);
    }

    // Determine entry type based on input/output counts
    // Standard: N inputs, N outputs (same denom)
    // Promotion: PROMOTION_RATIO inputs of session denom, 1 output of larger adjacent denom
    // Demotion: 1 input of larger adjacent denom, PROMOTION_RATIO outputs of session denom
    enum class EntryType { STANDARD, PROMOTION, DEMOTION, INVALID };
    EntryType entryType = EntryType::STANDARD;

    if (vin.size() == vout.size()) {
        entryType = EntryType::STANDARD;
    } else if (fV24Active) {
        if (vin.size() == static_cast<size_t>(CoinJoin::PROMOTION_RATIO) && vout.size() == 1) {
            entryType = EntryType::PROMOTION;
        } else if (vin.size() == 1 && vout.size() == static_cast<size_t>(CoinJoin::PROMOTION_RATIO)) {
            entryType = EntryType::DEMOTION;
        } else {
            entryType = EntryType::INVALID;
        }
    } else {
        // Pre-V24: only standard entries allowed
        LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::%s -- ERROR: inputs vs outputs size mismatch! %d vs %d\n", __func__, vin.size(), vout.size());
        nMessageIDRet = ERR_SIZE_MISMATCH;
        if (fConsumeCollateralRet) *fConsumeCollateralRet = true;
        return false;
    }

    if (entryType == EntryType::INVALID) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::%s -- ERROR: invalid entry structure! %d inputs, %d outputs\n", __func__, vin.size(), vout.size());
        nMessageIDRet = ERR_SIZE_MISMATCH;
        if (fConsumeCollateralRet) *fConsumeCollateralRet = true;
        return false;
    }

    const int nLargerAdjacentDenom = CoinJoin::GetLargerAdjacentDenom(nSessionDenom);

    // Determine expected denominations based on entry type
    int nExpectedInputDenom = nSessionDenom;
    int nExpectedOutputDenom = nSessionDenom;

    if (entryType == EntryType::PROMOTION) {
        // Promotion: inputs = session denom (smaller), output = larger adjacent
        nExpectedInputDenom = nSessionDenom;
        nExpectedOutputDenom = nLargerAdjacentDenom;
        if (nLargerAdjacentDenom == 0) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::%s -- ERROR: no larger adjacent denom for promotion\n", __func__);
            nMessageIDRet = ERR_DENOM;
            if (fConsumeCollateralRet) *fConsumeCollateralRet = true;
            return false;
        }
    } else if (entryType == EntryType::DEMOTION) {
        // Demotion: input = larger adjacent, outputs = session denom (smaller)
        nExpectedInputDenom = nLargerAdjacentDenom;
        nExpectedOutputDenom = nSessionDenom;
        if (nLargerAdjacentDenom == 0) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::%s -- ERROR: no larger adjacent denom for demotion\n", __func__);
            nMessageIDRet = ERR_DENOM;
            if (fConsumeCollateralRet) *fConsumeCollateralRet = true;
            return false;
        }
    }

    auto checkTxOut = [&](const CTxOut& txout, int nExpectedDenom) {
        const int nDenom = CoinJoin::AmountToDenomination(txout.nValue);

        if (nDenom != nExpectedDenom) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::IsValidInOuts -- ERROR: incompatible denom %d (%s) != expected %d (%s)\n",
                    nDenom, CoinJoin::DenominationToString(nDenom), nExpectedDenom, CoinJoin::DenominationToString(nExpectedDenom));
            nMessageIDRet = ERR_DENOM;
            if (fConsumeCollateralRet) *fConsumeCollateralRet = true;
            return false;
        }
        if (!txout.scriptPubKey.IsPayToPublicKeyHash()) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::IsValidInOuts -- ERROR: invalid script! scriptPubKey=%s\n", ScriptToAsmStr(txout.scriptPubKey));
            nMessageIDRet = ERR_INVALID_SCRIPT;
            if (fConsumeCollateralRet) *fConsumeCollateralRet = true;
            return false;
        }
        // Check for duplicate scripts across all inputs and outputs (privacy requirement)
        if (!setScripPubKeys.insert(txout.scriptPubKey).second) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::IsValidInOuts -- ERROR: already have this script! scriptPubKey=%s\n", ScriptToAsmStr(txout.scriptPubKey));
            nMessageIDRet = ERR_ALREADY_HAVE;
            if (fConsumeCollateralRet) *fConsumeCollateralRet = true;
            return false;
        }
        return true;
    };

    CAmount nFees{0};

    for (const auto& txout : vout) {
        if (!checkTxOut(txout, nExpectedOutputDenom)) {
            return false;
        }
        nFees -= txout.nValue;
    }

    CCoinsViewMemPool viewMemPool(WITH_LOCK(::cs_main, return &active_chainstate.CoinsTip()), mempool);

    for (const auto& txin : vin) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::%s -- txin=%s\n", __func__, txin.ToString());

        if (txin.prevout.IsNull()) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::%s -- ERROR: invalid input!\n", __func__);
            nMessageIDRet = ERR_INVALID_INPUT;
            if (fConsumeCollateralRet) *fConsumeCollateralRet = true;
            return false;
        }

        Coin coin;
        if (!viewMemPool.GetCoin(txin.prevout, coin) || coin.IsSpent() ||
            (coin.nHeight == MEMPOOL_HEIGHT && !isman.IsLocked(txin.prevout.hash))) {
            LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::%s -- ERROR: missing, spent or non-locked mempool input! txin=%s\n", __func__, txin.ToString());
            nMessageIDRet = ERR_MISSING_TX;
            return false;
        }

        if (!checkTxOut(coin.out, nExpectedInputDenom)) {
            return false;
        }

        nFees += coin.out.nValue;
    }

    // Value sum must match: inputs == outputs (no fees in CoinJoin)
    // This holds for standard mixing (same denom) and promotion/demotion (value preserved)
    if (nFees != 0) {
        LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::%s -- ERROR: non-zero fees! fees: %lld\n", __func__, nFees);
        nMessageIDRet = ERR_FEES;
        return false;
    }

    LogPrint(BCLog::COINJOIN, "CCoinJoinBaseSession::%s -- Valid %s entry: %d inputs, %d outputs\n",
             __func__,
             entryType == EntryType::PROMOTION ? "PROMOTION" : (entryType == EntryType::DEMOTION ? "DEMOTION" : "STANDARD"),
             vin.size(), vout.size());

    return true;
}

// Responsibility for checking fee sanity is moved from the mempool to the client (BroadcastTransaction)
// but CoinJoin still requires ATMP with fee sanity checks so we need to implement them separately
bool ATMPIfSaneFee(ChainstateManager& chainman, const CTransactionRef& tx, bool test_accept)
{
    AssertLockHeld(::cs_main);

    const MempoolAcceptResult result = chainman.ProcessTransaction(tx, /*test_accept=*/true);
    if (result.m_result_type != MempoolAcceptResult::ResultType::VALID) {
        /* Fetch fee and fast-fail if ATMP fails regardless */
        return false;
    } else if (result.m_base_fees.value() > DEFAULT_MAX_RAW_TX_FEE) {
        /* Check fee against fixed upper limit */
        return false;
    } else if (test_accept) {
        /* Don't re-run ATMP if only doing test run */
        return true;
    }
    return chainman.ProcessTransaction(tx, test_accept).m_result_type == MempoolAcceptResult::ResultType::VALID;
}

// check to make sure the collateral provided by the client is valid
bool CoinJoin::IsCollateralValid(ChainstateManager& chainman, const llmq::CInstantSendManager& isman,
                                 const CTxMemPool& mempool, const CTransaction& txCollateral)
{
    if (txCollateral.vout.empty()) return false;
    if (txCollateral.nLockTime != 0) return false;

    CAmount nValueIn = 0;
    CAmount nValueOut = 0;

    for (const auto& txout : txCollateral.vout) {
        nValueOut += txout.nValue;

        if (!txout.scriptPubKey.IsPayToPublicKeyHash() && !txout.scriptPubKey.IsUnspendable()) {
            LogPrint(BCLog::COINJOIN, "CoinJoin::IsCollateralValid -- Invalid Script, txCollateral=%s", txCollateral.ToString()); /* Continued */
            return false;
        }
    }

    LOCK(::cs_main);
    CCoinsViewMemPool viewMemPool(&chainman.ActiveChainstate().CoinsTip(), mempool);

    for (const auto& txin : txCollateral.vin) {
        Coin coin;
        if (!viewMemPool.GetCoin(txin.prevout, coin) || coin.IsSpent() ||
            (coin.nHeight == MEMPOOL_HEIGHT && !isman.IsLocked(txin.prevout.hash))) {
            LogPrint(BCLog::COINJOIN, "CoinJoin::IsCollateralValid -- missing, spent or non-locked mempool input! txin=%s\n", txin.ToString());
            return false;
        }
        nValueIn += coin.out.nValue;
    }

    //collateral transactions are required to pay out a small fee to the miners
    if (nValueIn - nValueOut < GetCollateralAmount()) {
        LogPrint(BCLog::COINJOIN, "CoinJoin::IsCollateralValid -- did not include enough fees in transaction: fees: %d, txCollateral=%s", nValueOut - nValueIn, txCollateral.ToString()); /* Continued */
        return false;
    }

    LogPrint(BCLog::COINJOIN, "CoinJoin::IsCollateralValid -- %s", txCollateral.ToString()); /* Continued */

    if (!ATMPIfSaneFee(chainman, MakeTransactionRef(txCollateral), /*test_accept=*/true)) {
        LogPrint(BCLog::COINJOIN, "CoinJoin::IsCollateralValid -- didn't pass ATMPIfSaneFee()\n");
        return false;
    }

    return true;
}

bilingual_str CoinJoin::GetMessageByID(PoolMessage nMessageID)
{
    switch (nMessageID) {
    case ERR_ALREADY_HAVE:
        return _("Already have that input.");
    case ERR_DENOM:
        return _("No matching denominations found for mixing.");
    case ERR_ENTRIES_FULL:
        return _("Entries are full.");
    case ERR_EXISTING_TX:
        return _("Not compatible with existing transactions.");
    case ERR_FEES:
        return _("Transaction fees are too high.");
    case ERR_INVALID_COLLATERAL:
        return _("Collateral not valid.");
    case ERR_INVALID_INPUT:
        return _("Input is not valid.");
    case ERR_INVALID_SCRIPT:
        return _("Invalid script detected.");
    case ERR_INVALID_TX:
        return _("Transaction not valid.");
    case ERR_MAXIMUM:
        return _("Entry exceeds maximum size.");
    case ERR_MN_LIST:
        return _("Not in the Masternode list.");
    case ERR_MODE:
        return _("Incompatible mode.");
    case ERR_QUEUE_FULL:
        return _("Masternode queue is full.");
    case ERR_RECENT:
        return _("Last queue was created too recently.");
    case ERR_SESSION:
        return _("Session not complete!");
    case ERR_MISSING_TX:
        return _("Missing input transaction information.");
    case ERR_VERSION:
        return _("Incompatible version.");
    case MSG_NOERR:
        return _("No errors detected.");
    case MSG_SUCCESS:
        return _("Transaction created successfully.");
    case MSG_ENTRIES_ADDED:
        return _("Your entries added successfully.");
    case ERR_SIZE_MISMATCH:
        return _("Inputs vs outputs size mismatch.");
    case ERR_NON_STANDARD_PUBKEY:
    case ERR_NOT_A_MN:
    default:
        return _("Unknown response.");
    }
}

CDSTXManager::CDSTXManager(const chainlock::Chainlocks& chainlocks) :
    m_chainlocks{chainlocks}
{
}
CDSTXManager::~CDSTXManager() = default;

void CDSTXManager::AddDSTX(const CCoinJoinBroadcastTx& dstx)
{
    AssertLockNotHeld(cs_mapdstx);
    LOCK(cs_mapdstx);
    mapDSTX.insert(std::make_pair(dstx.tx->GetHash(), dstx));
}

CCoinJoinBroadcastTx CDSTXManager::GetDSTX(const uint256& hash)
{
    AssertLockNotHeld(cs_mapdstx);
    LOCK(cs_mapdstx);
    auto it = mapDSTX.find(hash);
    return (it == mapDSTX.end()) ? CCoinJoinBroadcastTx() : it->second;
}

bool CDSTXManager::IsTxExpired(const CCoinJoinBroadcastTx& tx, const CBlockIndex* pindex) const
{
    // expire confirmed DSTXes after ~1h since confirmation or chainlocked
    const auto& opt_confirmed_height = tx.GetConfirmedHeight();
    if (!opt_confirmed_height.has_value() || pindex->nHeight < *opt_confirmed_height) return false; // not mined yet
    return (pindex->nHeight - *opt_confirmed_height > 24) ||
           m_chainlocks.HasChainLock(pindex->nHeight, *pindex->phashBlock); // mined more than an hour ago or chainlocked
}

void CDSTXManager::CheckDSTXes(const CBlockIndex* pindex)
{
    AssertLockNotHeld(cs_mapdstx);
    LOCK(cs_mapdstx);
    auto it = mapDSTX.begin();
    while (it != mapDSTX.end()) {
        if (IsTxExpired(it->second, pindex)) {
            mapDSTX.erase(it++);
        } else {
            ++it;
        }
    }
    LogPrint(BCLog::COINJOIN, "CoinJoin::CheckDSTXes -- mapDSTX.size()=%llu\n", mapDSTX.size());
}

void CDSTXManager::UpdatedBlockTip(const CBlockIndex* pindex)
{
    if (pindex) {
        CheckDSTXes(pindex);
    }
}

void CDSTXManager::NotifyChainLock(const CBlockIndex* pindex)
{
    if (pindex) {
        CheckDSTXes(pindex);
    }
}

void CDSTXManager::UpdateDSTXConfirmedHeight(const CTransactionRef& tx, std::optional<int> nHeight)
{
    AssertLockHeld(cs_mapdstx);

    auto it = mapDSTX.find(tx->GetHash());
    if (it == mapDSTX.end()) {
        return;
    }

    it->second.SetConfirmedHeight(nHeight);
    LogPrint(BCLog::COINJOIN, "CDSTXManager::%s -- txid=%s, nHeight=%d\n", __func__, tx->GetHash().ToString(), nHeight.value_or(-1));
}

void CDSTXManager::TransactionAddedToMempool(const CTransactionRef& tx)
{
    AssertLockNotHeld(cs_mapdstx);
    LOCK(cs_mapdstx);
    UpdateDSTXConfirmedHeight(tx, std::nullopt);
}

void CDSTXManager::BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex)
{
    AssertLockNotHeld(cs_mapdstx);
    LOCK(cs_mapdstx);

    for (const auto& tx : pblock->vtx) {
        UpdateDSTXConfirmedHeight(tx, pindex->nHeight);
    }
}

void CDSTXManager::BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex*)
{
    AssertLockNotHeld(cs_mapdstx);
    LOCK(cs_mapdstx);
    for (const auto& tx : pblock->vtx) {
        UpdateDSTXConfirmedHeight(tx, std::nullopt);
    }
}

int CoinJoin::GetMinPoolParticipants() { return Params().PoolMinParticipants(); }
int CoinJoin::GetMaxPoolParticipants() { return Params().PoolMaxParticipants(); }

bool CoinJoin::ValidatePromotionEntry(const std::vector<CTxIn>& vecTxIn, const std::vector<CTxOut>& vecTxOut,
                                       int nSessionDenom, PoolMessage& nMessageIDRet)
{
    // Promotion: 10 inputs of smaller denom → 1 output of larger denom
    // Session denom is the smaller denom (inputs)
    nMessageIDRet = MSG_NOERR;

    // Check input count
    if (vecTxIn.size() != static_cast<size_t>(PROMOTION_RATIO)) {
        LogPrint(BCLog::COINJOIN, "CoinJoin::ValidatePromotionEntry -- ERROR: wrong input count %zu, expected %d\n",
                vecTxIn.size(), PROMOTION_RATIO);
        nMessageIDRet = ERR_SIZE_MISMATCH;
        return false;
    }

    // Check output count
    if (vecTxOut.size() != 1) {
        LogPrint(BCLog::COINJOIN, "CoinJoin::ValidatePromotionEntry -- ERROR: wrong output count %zu, expected 1\n",
                vecTxOut.size());
        nMessageIDRet = ERR_SIZE_MISMATCH;
        return false;
    }

    // Get the larger adjacent denomination
    const int nLargerDenom = GetLargerAdjacentDenom(nSessionDenom);
    if (nLargerDenom == 0) {
        LogPrint(BCLog::COINJOIN, "CoinJoin::ValidatePromotionEntry -- ERROR: no larger adjacent denom for %s\n",
                DenominationToString(nSessionDenom));
        nMessageIDRet = ERR_DENOM;
        return false;
    }

    // Validate output is at larger denomination
    const int nOutputDenom = AmountToDenomination(vecTxOut[0].nValue);
    if (nOutputDenom != nLargerDenom) {
        LogPrint(BCLog::COINJOIN, "CoinJoin::ValidatePromotionEntry -- ERROR: output denom %s != expected %s\n",
                DenominationToString(nOutputDenom), DenominationToString(nLargerDenom));
        nMessageIDRet = ERR_DENOM;
        return false;
    }

    // Validate output is P2PKH
    if (!vecTxOut[0].scriptPubKey.IsPayToPublicKeyHash()) {
        LogPrint(BCLog::COINJOIN, "CoinJoin::ValidatePromotionEntry -- ERROR: output is not P2PKH\n");
        nMessageIDRet = ERR_INVALID_SCRIPT;
        return false;
    }

    return true;
}

bool CoinJoin::ValidateDemotionEntry(const std::vector<CTxIn>& vecTxIn, const std::vector<CTxOut>& vecTxOut,
                                      int nSessionDenom, PoolMessage& nMessageIDRet)
{
    // Demotion: 1 input of larger denom → 10 outputs of smaller denom
    // Session denom is the smaller denom (outputs)
    nMessageIDRet = MSG_NOERR;

    // Check input count
    if (vecTxIn.size() != 1) {
        LogPrint(BCLog::COINJOIN, "CoinJoin::ValidateDemotionEntry -- ERROR: wrong input count %zu, expected 1\n",
                vecTxIn.size());
        nMessageIDRet = ERR_SIZE_MISMATCH;
        return false;
    }

    // Check output count
    if (vecTxOut.size() != static_cast<size_t>(PROMOTION_RATIO)) {
        LogPrint(BCLog::COINJOIN, "CoinJoin::ValidateDemotionEntry -- ERROR: wrong output count %zu, expected %d\n",
                vecTxOut.size(), PROMOTION_RATIO);
        nMessageIDRet = ERR_SIZE_MISMATCH;
        return false;
    }

    // Validate all outputs are at session denomination and P2PKH
    for (const auto& txout : vecTxOut) {
        const int nDenom = AmountToDenomination(txout.nValue);
        if (nDenom != nSessionDenom) {
            LogPrint(BCLog::COINJOIN, "CoinJoin::ValidateDemotionEntry -- ERROR: output denom %s != session denom %s\n",
                    DenominationToString(nDenom), DenominationToString(nSessionDenom));
            nMessageIDRet = ERR_DENOM;
            return false;
        }
        if (!txout.scriptPubKey.IsPayToPublicKeyHash()) {
            LogPrint(BCLog::COINJOIN, "CoinJoin::ValidateDemotionEntry -- ERROR: output is not P2PKH\n");
            nMessageIDRet = ERR_INVALID_SCRIPT;
            return false;
        }
    }

    return true;
}
