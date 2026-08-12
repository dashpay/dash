// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/masternode.h>
#include <bls/bls.h>
#include <chain.h>
#include <coinjoin/coinjoin.h>
#include <coinjoin/common.h>
#include <coinjoin/server.h>
#include <consensus/amount.h>
#include <evo/chainhelper.h>
#include <llmq/context.h>
#include <masternode/sync.h>
#include <net.h>
#include <node/connection_types.h>
#include <protocol.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(coinjoin_inouts_tests, TestingSetup)

static CBLSSecretKey MakeSecretKey()
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    return sk;
}

static CScript P2PKHScript(uint8_t tag = 0x01)
{
    // OP_DUP OP_HASH160 <20-byte-tag> OP_EQUALVERIFY OP_CHECKSIG
    std::vector<unsigned char> hash(20, tag);
    return CScript{} << OP_DUP << OP_HASH160 << hash << OP_EQUALVERIFY << OP_CHECKSIG;
}

BOOST_AUTO_TEST_CASE(broadcasttx_isvalidstructure_good_and_bad)
{
    // Good: equal vin/vout sizes, vin count >= min participants, <= max*entry_size, P2PKH outputs with standard denominations
    CCoinJoinBroadcastTx good;
    {
        CMutableTransaction mtx;
        // Use min pool participants (e.g. 3). Build 3 inputs and 3 denominated outputs
        const int participants = std::max(3, CoinJoin::GetMinPoolParticipants());
        for (int i = 0; i < participants; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
            // Pick the smallest denomination
            CTxOut out{CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i))};
            mtx.vout.push_back(out);
        }
        good.tx = MakeTransactionRef(mtx);
        good.m_protxHash = uint256::ONE; // at least one of (outpoint, protxhash) must be set
    }
    BOOST_CHECK(good.IsValidStructure());

    // Bad: both identifiers null
    CCoinJoinBroadcastTx bad_ids = good;
    bad_ids.m_protxHash = uint256{};
    bad_ids.masternodeOutpoint.SetNull();
    BOOST_CHECK(!bad_ids.IsValidStructure());

    // Bad: vin/vout size mismatch
    CCoinJoinBroadcastTx bad_sizes = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout.pop_back();
        bad_sizes.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_sizes.IsValidStructure());

    // Bad: non-P2PKH output
    CCoinJoinBroadcastTx bad_script = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>{'x'};
        bad_script.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_script.IsValidStructure());

    // Bad: non-denominated amount
    CCoinJoinBroadcastTx bad_amount = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout[0].nValue = 42; // not a valid denom
        bad_amount.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_amount.IsValidStructure());
}

BOOST_AUTO_TEST_CASE(entry_addscriptsig_matches_and_rejects)
{
    // Build an entry with two distinct inputs so we can check both the match
    // and the isolation (only the matching input mutates).
    const COutPoint op0(uint256::ONE, 0);
    const COutPoint op1(uint256::ONE, 1);
    const uint32_t seq0 = 0xfffffffeU;
    const uint32_t seq1 = 0xfffffffdU;

    auto make_dsin = [](const COutPoint& op, uint32_t seq) {
        CTxIn in(op);
        in.nSequence = seq;
        return CTxDSIn(in, P2PKHScript(0x10), /*nRounds=*/0);
    };

    std::vector<CTxDSIn> dsins{make_dsin(op0, seq0), make_dsin(op1, seq1)};
    CCoinJoinEntry entry(dsins, /*vecTxOut=*/{}, CTransaction{CMutableTransaction{}});

    // The scriptSig we expect to be copied across on a successful match.
    const CScript scriptSig0 = CScript() << std::vector<unsigned char>{0xde, 0xad} << std::vector<unsigned char>{0xbe, 0xef};

    // Matching prevout + matching sequence -> copies scriptSig, sets fHasSig.
    {
        CTxIn signed_in(op0, scriptSig0, seq0);
        BOOST_CHECK(entry.AddScriptSig(signed_in));
        BOOST_CHECK(entry.vecTxDSIn[0].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[0].scriptSig == scriptSig0);
        // Other input is untouched.
        BOOST_CHECK(!entry.vecTxDSIn[1].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[1].scriptSig.empty());
    }

    // Duplicate signature for the already-signed input -> rejected, no overwrite.
    {
        const CScript scriptSig_other = CScript() << std::vector<unsigned char>{0x01};
        CTxIn dup_in(op0, scriptSig_other, seq0);
        BOOST_CHECK(!entry.AddScriptSig(dup_in));
        // Still holds the original signature.
        BOOST_CHECK(entry.vecTxDSIn[0].scriptSig == scriptSig0);
        BOOST_CHECK(entry.vecTxDSIn[0].fHasSig);
    }

    // Wrong prevout (sequence matches an existing input) -> rejected.
    {
        const COutPoint op_wrong(uint256S("ff"), 9);
        CTxIn wrong_in(op_wrong, scriptSig0, seq1);
        BOOST_CHECK(!entry.AddScriptSig(wrong_in));
        BOOST_CHECK(!entry.vecTxDSIn[1].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[1].scriptSig.empty());
    }

    // Right prevout but wrong sequence -> rejected (guards against malleated nSequence).
    {
        CTxIn badseq_in(op1, scriptSig0, /*nSequence=*/seq1 ^ 0xffU);
        BOOST_CHECK(!entry.AddScriptSig(badseq_in));
        BOOST_CHECK(!entry.vecTxDSIn[1].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[1].scriptSig.empty());
    }

    // Correct prevout + correct sequence on the second input -> succeeds, doesn't disturb the first.
    {
        const CScript scriptSig1 = CScript() << std::vector<unsigned char>{0xca, 0xfe};
        CTxIn signed_in(op1, scriptSig1, seq1);
        BOOST_CHECK(entry.AddScriptSig(signed_in));
        BOOST_CHECK(entry.vecTxDSIn[1].fHasSig);
        BOOST_CHECK(entry.vecTxDSIn[1].scriptSig == scriptSig1);
        // First input unchanged.
        BOOST_CHECK(entry.vecTxDSIn[0].scriptSig == scriptSig0);
    }
}

// Test-only subclass exposing the minimal seams needed to exercise server lifecycle behavior
// without standing up a full DKG-backed signing session. The helpers only establish preconditions
// and invoke the production paths; the behavior under test is not reproduced here.
class TestableCoinJoinServer : public CCoinJoinServer
{
public:
    using CCoinJoinServer::CCoinJoinServer;

    mutable std::vector<CTransactionRef> consumed_collaterals;

    void ConsumeCollateral(const CTransactionRef& txref) const override { consumed_collaterals.push_back(txref); }

    void ResetForTest(PoolState state)
    {
        LOCK(cs_coinjoin);
        SetNull();
        nState = state;
        nSessionID = 1;
        consumed_collaterals.clear();
    }

    void SetTimedOutForTest()
    {
        LOCK(cs_coinjoin);
        nTimeLastSuccessfulStep = GetTime() -
            ((nState == POOL_STATE_SIGNING) ? COINJOIN_SIGNING_TIMEOUT : COINJOIN_QUEUE_TIMEOUT);
    }

    void AddCollateralForTest(const CTransactionRef& collateral)
    {
        LOCK(cs_coinjoin);
        m_session_collaterals.Add(CMutableTransaction{*collateral});
    }

    void AddEntryForTest(const CCoinJoinEntry& entry)
    {
        LOCK(cs_coinjoin);
        vecEntries.push_back(entry);
    }

    CTransactionRef SelectForTest(FeePolicy policy)
    {
        LOCK(cs_coinjoin);
        return SelectCollateralToCharge(policy);
    }

    void ChargeRandomFeesForTest() const
    {
        std::vector<CTransactionRef> collaterals;
        WITH_LOCK(cs_coinjoin, collaterals = m_session_collaterals.txs());
        ChargeRandomFees(collaterals);
    }

    void EnterSigningState()
    {
        LOCK(cs_coinjoin);
        SetState(POOL_STATE_SIGNING);
    }

    //! Offer a collateral to CreateNewSession() the way a DSACCEPT would. fUnitTest skips the
    //! mempool-backed collateral validity check and the dsq relay, isolating admission logic.
    bool TryAdmit(const CTransactionRef& collateral, PoolMessage& message) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        fUnitTest = true;
        const CCoinJoinAccept dsa{CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination()),
                                  CMutableTransaction{*collateral}};
        return CreateNewSession(dsa, message);
    }

    void SetFinalTransactionForTest(const CMutableTransaction& tx)
    {
        LOCK(cs_coinjoin);
        finalMutableTransaction = tx;
    }

    void CheckPoolForTest() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin, !cs_check_pool) { CheckPool(); }

    void RelayAbortForTest() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        RelayStatus(STATUS_REJECTED);
    }

    //! Models the tail reset a committing CheckPool() performs concurrently.
    void ClearPoolForTest() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        SetNull();
    }

    //! Models CheckForCompleteQueue()'s transition, which does not touch abort state.
    void EnterAcceptingEntriesState() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        SetState(POOL_STATE_ACCEPTING_ENTRIES);
    }

    bool RelayedAbortForTest() const EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        return m_relayed_abort;
    }

    bool RealAddScriptSig(const CTxIn& txin) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        return CCoinJoinServer::AddScriptSig(txin);
    }

    void SeedParticipant(const CService& addr) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        CCoinJoinEntry entry;
        entry.addr = addr;
        LOCK(cs_coinjoin);
        vecEntries.push_back(std::move(entry));
    }

    void SeedCompletionSession(int session_id, const CService& addr, PoolState state = POOL_STATE_SIGNING)
    {
        LOCK(cs_coinjoin);
        SetNull();
        nSessionID = session_id;
        nState = state;

        if (state == POOL_STATE_SIGNING) {
            CCoinJoinEntry entry;
            entry.addr = addr;
            vecEntries.push_back(std::move(entry));
        }
    }

    void RelayCompletion(int session_id, const CService& addr)
    {
        RelayCompletedTransaction(session_id, {addr}, MSG_SUCCESS);
    }

    void ResetForSession(int session_id) { ResetSigningSessionIfCurrent(session_id); }

    void SeedTimedOutSession()
    {
        LOCK(cs_coinjoin);
        nSessionID = 1;
        nState = POOL_STATE_ACCEPTING_ENTRIES;
        nTimeLastSuccessfulStep = GetTime() - COINJOIN_QUEUE_TIMEOUT;
        for (int i = 0; i < CoinJoin::GetMinPoolParticipants(); ++i) {
            CMutableTransaction collateral;
            collateral.vin.emplace_back(COutPoint{uint256::ONE, static_cast<uint32_t>(i)});
            m_session_collaterals.Add(collateral);
        }
    }

    void SeedTimedOutActionableSession(PoolState state, bool has_missing_entry = false)
    {
        LOCK(cs_coinjoin);
        SetNull();

        nSessionID = 1;
        nState = state;
        nTimeLastSuccessfulStep = GetTime() -
                                  (state == POOL_STATE_SIGNING ? COINJOIN_SIGNING_TIMEOUT : COINJOIN_QUEUE_TIMEOUT);

        for (int i = 0; i < CoinJoin::GetMinPoolParticipants(); ++i) {
            CMutableTransaction collateral;
            collateral.vin.emplace_back(COutPoint{uint256::ONE, static_cast<uint32_t>(i)});
            m_session_collaterals.Add(collateral);

            if (state == POOL_STATE_QUEUE) continue;

            CTxDSIn txdsin{CTxIn{COutPoint{uint256::TWO, static_cast<uint32_t>(i)}}, P2PKHScript(), 0};
            txdsin.fHasSig = state == POOL_STATE_SIGNING;
            vecEntries.emplace_back(std::vector<CTxDSIn>{txdsin}, std::vector<CTxOut>{}, CTransaction{collateral});
        }

        if (has_missing_entry) {
            CMutableTransaction collateral;
            collateral.vin.emplace_back(COutPoint{uint256::ONE, static_cast<uint32_t>(CoinJoin::GetMinPoolParticipants())});
            m_session_collaterals.Add(collateral);
        }
    }

    void HoldPoolCheck(std::latch& locked, std::latch& release)
    {
        LOCK(cs_check_pool);
        locked.count_down();
        release.wait();
    }

    int MarkMessageInFlightForTest()
    {
        LOCK(cs_coinjoin);
        return MarkMessageInFlight();
    }

    void ClearMessageInFlightForTest(int session_id) { ClearMessageInFlight(session_id); }

    bool ValidateInOuts(const std::vector<CTxIn>& vin, const std::vector<CTxOut>& vout, int session_denom,
                        PoolMessage& message, bool& consume_collateral)
    {
        return IsValidInOuts(m_chainman.ActiveChainstate(), m_isman, mempool, vin, vout, session_denom, message,
                             &consume_collateral);
    }
};

static CTransactionRef MakeCollateral(uint32_t id)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{uint256::ONE, id});
    tx.vout.emplace_back(CoinJoin::GetCollateralAmount(), P2PKHScript(static_cast<uint8_t>(id)));
    return MakeTransactionRef(tx);
}

static CCoinJoinEntry MakeEntry(const CTransactionRef& collateral, size_t unsigned_inputs = 0)
{
    std::vector<CTxDSIn> inputs;
    for (size_t i = 0; i < std::max<size_t>(unsigned_inputs, 1); ++i) {
        CTxDSIn input{CTxIn{COutPoint{uint256::ONE, static_cast<uint32_t>(100 + i)}}, P2PKHScript(1), 0};
        input.fHasSig = unsigned_inputs == 0;
        inputs.push_back(input);
    }
    return CCoinJoinEntry{inputs, {}, CTransaction{*collateral}};
}

BOOST_AUTO_TEST_CASE(server_abort_fee_selects_unique_offenders)
{
    BOOST_REQUIRE(m_node.mn_sync);
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));

    // A queue that never became ready has no identifiable offender and charges nobody.
    server.ResetForTest(POOL_STATE_QUEUE);
    server.SetTimedOutForTest();
    server.CheckTimeout();
    BOOST_CHECK(server.consumed_collaterals.empty());

    // All 20 reservations failed to submit. The abort policy still selects exactly one.
    std::vector<CTransactionRef> collaterals;
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (uint32_t i = 0; i < 20; ++i) {
        collaterals.push_back(MakeCollateral(i));
        server.AddCollateralForTest(collaterals.back());
    }
    BOOST_CHECK(server.SelectForTest(CCoinJoinServer::FeePolicy::PROBABILISTIC) == nullptr);
    BOOST_CHECK(server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT) != nullptr);
    server.SetTimedOutForTest();
    server.CheckTimeout();
    BOOST_CHECK_EQUAL(server.consumed_collaterals.size(), 1U);

    // With one submission, only one of the other 19 reservations can be selected.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (const auto& collateral : collaterals) server.AddCollateralForTest(collateral);
    server.AddEntryForTest(MakeEntry(collaterals[0]));
    for (int i = 0; i < 64; ++i) {
        const auto selected = server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
        BOOST_REQUIRE(selected);
        BOOST_CHECK(*selected != *collaterals[0]);
    }

    // Three of five submitted, so only the two missing reservations are eligible.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (size_t i = 0; i < 5; ++i) server.AddCollateralForTest(collaterals[i]);
    for (size_t i = 0; i < 3; ++i) server.AddEntryForTest(MakeEntry(collaterals[i]));
    for (int i = 0; i < 64; ++i) {
        const auto selected = server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
        BOOST_REQUIRE(selected);
        BOOST_CHECK(*selected == *collaterals[3] || *selected == *collaterals[4]);
    }

    // If every reservation submitted, there is no missing-entry collateral to select.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (size_t i = 0; i < 5; ++i) {
        server.AddCollateralForTest(collaterals[i]);
        server.AddEntryForTest(MakeEntry(collaterals[i]));
    }
    BOOST_CHECK(server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT) == nullptr);

    // A lone non-signer is selected deterministically.
    server.ResetForTest(POOL_STATE_SIGNING);
    for (size_t i = 0; i < 3; ++i) {
        server.AddCollateralForTest(collaterals[i]);
        server.AddEntryForTest(MakeEntry(collaterals[i], i == 2 ? 1 : 0));
    }
    const auto lone_non_signer = server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
    BOOST_REQUIRE(lone_non_signer);
    BOOST_CHECK(*lone_non_signer == *collaterals[2]);

    // Multiple non-signers select one member of that set; unsigned input count gives no extra weight.
    server.ResetForTest(POOL_STATE_SIGNING);
    server.AddCollateralForTest(collaterals[0]);
    server.AddEntryForTest(MakeEntry(collaterals[0], 8));
    server.AddCollateralForTest(collaterals[1]);
    server.AddEntryForTest(MakeEntry(collaterals[1], 1));
    size_t selected_first{0};
    size_t selected_second{0};
    for (int i = 0; i < 256; ++i) {
        const auto selected = server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
        BOOST_REQUIRE(selected);
        if (*selected == *collaterals[0]) ++selected_first;
        if (*selected == *collaterals[1]) ++selected_second;
    }
    BOOST_CHECK_GT(selected_first, 64U);
    BOOST_CHECK_GT(selected_second, 64U);

    // When every participant fails to sign, the abort policy still chooses exactly one.
    server.SetTimedOutForTest();
    server.CheckTimeout();
    BOOST_CHECK_EQUAL(server.consumed_collaterals.size(), 1U);

    // A recoverable timeout retains the old probabilistic gate.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (size_t i = 0; i < 5; ++i) server.AddCollateralForTest(collaterals[i]);
    for (size_t i = 0; i < 3; ++i) server.AddEntryForTest(MakeEntry(collaterals[i]));
    bool selected_recoverable{false};
    bool skipped_recoverable{false};
    for (int i = 0; i < 256; ++i) {
        if (server.SelectForTest(CCoinJoinServer::FeePolicy::PROBABILISTIC)) {
            selected_recoverable = true;
        } else {
            skipped_recoverable = true;
        }
    }
    BOOST_CHECK(selected_recoverable);
    BOOST_CHECK(skipped_recoverable);

    // Successful-session random charging remains independently probabilistic.
    server.ResetForTest(POOL_STATE_SIGNING);
    for (size_t i = 0; i < 5; ++i) server.AddCollateralForTest(collaterals[i]);
    bool random_charge{false};
    bool random_skip{false};
    for (int i = 0; i < 256; ++i) {
        server.consumed_collaterals.clear();
        server.ChargeRandomFeesForTest();
        random_charge |= !server.consumed_collaterals.empty();
        random_skip |= server.consumed_collaterals.empty();
    }
    BOOST_CHECK(random_charge);
    BOOST_CHECK(random_skip);
}

BOOST_AUTO_TEST_CASE(server_timeout_reset_precedes_collateral_consumption)
{
    BOOST_REQUIRE(m_node.mn_sync);
    if (!m_node.mn_sync->IsBlockchainSynced()) m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    class SessionReplacingServer : public TestableCoinJoinServer
    {
    public:
        using TestableCoinJoinServer::TestableCoinJoinServer;
        mutable PoolState state_during_consume{POOL_STATE_ERROR};

        void ConsumeCollateral(const CTransactionRef& txref) const override
        {
            state_during_consume = static_cast<PoolState>(GetState());
            auto* self = const_cast<SessionReplacingServer*>(this);
            self->ResetForTest(POOL_STATE_QUEUE);
            TestableCoinJoinServer::ConsumeCollateral(txref);
        }
    };

    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    SessionReplacingServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
    const auto collateral = MakeCollateral(0);
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    server.AddCollateralForTest(collateral);
    server.SetTimedOutForTest();

    server.CheckTimeout();

    BOOST_CHECK(server.state_during_consume == POOL_STATE_IDLE);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_QUEUE});
    BOOST_CHECK_EQUAL(server.consumed_collaterals.size(), 1U);
}

BOOST_AUTO_TEST_CASE(server_pending_charge_blocks_readmission_until_consumed)
{
    // The timeout reset reopens admission before the penalty spend reaches the mempool. In that
    // window the selected collateral is still unspent and would pass every other admission check;
    // committing it to a replacement session would strand that session on a reservation whose
    // funds are already promised to the penalty.
    class ReadmissionRacingServer : public TestableCoinJoinServer
    {
    public:
        using TestableCoinJoinServer::TestableCoinJoinServer;
        mutable int readmitted_during_consume{-1};
        mutable PoolMessage readmission_message{MSG_NOERR};

        void ConsumeCollateral(const CTransactionRef& txref) const override
        {
            auto* self = const_cast<ReadmissionRacingServer*>(this);
            PoolMessage message{MSG_NOERR};
            readmitted_during_consume = self->TryAdmit(txref, message) ? 1 : 0;
            readmission_message = message;
            TestableCoinJoinServer::ConsumeCollateral(txref);
        }
    };

    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    ReadmissionRacingServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                   *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                   *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                   *Assert(m_node.llmq_ctx->isman));
    const auto collateral = MakeCollateral(0);
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    server.AddCollateralForTest(collateral);
    server.SetTimedOutForTest();

    server.CheckTimeout();

    // The pool was already reset when the consume ran, yet the pending charge kept the collateral
    // out of a replacement session.
    BOOST_REQUIRE_EQUAL(server.readmitted_during_consume, 0);
    BOOST_CHECK_EQUAL(server.readmission_message, ERR_INVALID_COLLATERAL);
    BOOST_CHECK_EQUAL(server.consumed_collaterals.size(), 1U);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});

    // Once the charge has settled the reservation is released and admission works again; in
    // production the mempool, which now contains the penalty spend, takes over rejecting it.
    PoolMessage message{MSG_NOERR};
    BOOST_CHECK(server.TryAdmit(collateral, message));
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_QUEUE});
}

BOOST_AUTO_TEST_CASE(server_timeout_waits_for_in_flight_messages)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
    const auto collateral = MakeCollateral(0);

    for (const PoolState state : {POOL_STATE_ACCEPTING_ENTRIES, POOL_STATE_SIGNING}) {
        server.ResetForTest(state);
        server.AddCollateralForTest(collateral);
        if (state == POOL_STATE_SIGNING) {
            server.AddEntryForTest(MakeEntry(collateral, /*unsigned_inputs=*/1));
        }

        const int session_id = server.MarkMessageInFlightForTest();
        server.SetTimedOutForTest();
        server.CheckTimeout();

        BOOST_CHECK_EQUAL(server.GetState(), int{state});
        BOOST_CHECK(server.consumed_collaterals.empty());

        server.ClearMessageInFlightForTest(session_id);
        server.CheckTimeout();

        BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
        BOOST_CHECK_EQUAL(server.consumed_collaterals.size(), 1U);
    }
}

static std::unique_ptr<CNode> MakePeer(NodeId id, uint32_t ipv4)
{
    in_addr peer_in_addr{};
    peer_in_addr.s_addr = htonl(ipv4);
    auto peer = std::make_unique<CNode>(id,
                                        /*sock=*/nullptr,
                                        /*addrIn=*/CAddress{CService{peer_in_addr, 8333}, NODE_NETWORK},
                                        /*nKeyedNetGroupIn=*/0,
                                        /*nLocalHostNonceIn=*/0,
                                        /*addrBindIn=*/CAddress{},
                                        /*addrNameIn=*/std::string{},
                                        /*conn_type_in=*/ConnectionType::INBOUND,
                                        /*inbound_onion=*/false);
    peer->nVersion = PROTOCOL_VERSION;
    peer->SetCommonVersion(PROTOCOL_VERSION);
    return peer;
}

BOOST_AUTO_TEST_CASE(server_signfinaltx_nonparticipant_cannot_abort_session)
{
    BOOST_REQUIRE(m_node.mn_sync);
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));

    // Seed an active signing session with one participant. That participant's
    // addr is deliberately not registered with connman -- a session-wide
    // RelayStatus(REJECTED) would therefore see nDisconnected == vecEntries.size()
    // and reset the pool state to POOL_STATE_IDLE via SetNull().
    auto participant = MakePeer(/*id=*/7, /*ipv4=*/0x0a000001);
    server.SeedParticipant(participant->addr);
    server.EnterSigningState();
    BOOST_REQUIRE_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});

    auto nonparticipant = MakePeer(/*id=*/42, /*ipv4=*/0x01020304);
    BOOST_REQUIRE(!(nonparticipant->addr == participant->addr));

    const size_t max_txins{CoinJoin::GetMaxPoolInputOutputCount()};
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(stream, max_txins + 1);

    BOOST_CHECK_NO_THROW(server.ProcessMessage(*nonparticipant, NetMsgType::DSSIGNFINALTX, stream));

    // The non-participant must be rejected without touching session-wide state:
    // the stream body must not have been read past the compact-size prefix, and
    // the pool must still be in POOL_STATE_SIGNING with its entry intact.
    BOOST_CHECK_EQUAL(stream.size(), GetSizeOfCompactSize(max_txins + 1));
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 1);
}

BOOST_AUTO_TEST_CASE(server_signfinaltx_participant_oversized_count_is_rejected_locally)
{
    BOOST_REQUIRE(m_node.mn_sync);
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));

    // Same setup, but this time the oversized DSSIGNFINALTX comes from the
    // session participant itself. It must still be rejected without materializing
    // the txin vector and without collapsing the session for everyone else.
    auto participant = MakePeer(/*id=*/7, /*ipv4=*/0x0a000001);
    server.SeedParticipant(participant->addr);
    server.EnterSigningState();

    const size_t max_txins{CoinJoin::GetMaxPoolInputOutputCount()};
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(stream, max_txins + 1);

    BOOST_CHECK_NO_THROW(server.ProcessMessage(*participant, NetMsgType::DSSIGNFINALTX, stream));
    BOOST_CHECK_EQUAL(stream.size(), 0U);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 1);

    // A count beyond the generic CompactSize cap is a malformed message, not a
    // CoinJoin-level violation: it throws the standard deserialization error out
    // to net processing. No txin is materialized and the session is left intact,
    // so honest participants are unaffected.
    CDataStream huge_stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(huge_stream, uint64_t{MAX_SIZE} + 1);

    BOOST_CHECK_THROW(server.ProcessMessage(*participant, NetMsgType::DSSIGNFINALTX, huge_stream),
                      std::ios_base::failure);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 1);
}

BOOST_AUTO_TEST_CASE(server_completion_does_not_reset_an_unreachable_or_replacement_session)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));

    auto participant = MakePeer(/*id=*/7, /*ipv4=*/0x0a000001);
    server.SeedCompletionSession(/*session_id=*/1, participant->addr);

    // The participant is deliberately absent from connman. Failing to deliver DSCOMPLETE must not
    // reset live session data; only CommitFinalTransaction owns the completion reset.
    server.RelayCompletion(/*session_id=*/1, participant->addr);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 1);

    // A delayed tail operation from session 1 must never clear a replacement queue, even if the
    // random wire session ID happens to be reused.
    server.SeedCompletionSession(/*session_id=*/1, participant->addr, POOL_STATE_QUEUE);
    server.ResetForSession(/*session_id=*/1);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_QUEUE});
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 0);
}

BOOST_AUTO_TEST_CASE(server_timeout_defers_and_commits_fully_signed_session)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));

    // A fully signed session whose committing CheckPool() round was skipped: the final signature
    // arrived while the scheduler held cs_check_pool, so its DSSIGNFINALTX could not commit, and
    // the scheduler's own sample predated the signature. CheckTimeout() then runs past the
    // deadline and must not treat the session as failed.
    const auto collateral = MakeCollateral(0);
    server.ResetForTest(POOL_STATE_SIGNING);
    server.AddCollateralForTest(collateral);
    server.AddEntryForTest(MakeEntry(collateral, /*unsigned_inputs=*/0));

    CMutableTransaction final_tx;
    final_tx.vin.emplace_back(COutPoint{uint256::ONE, 100});
    server.SetFinalTransactionForTest(final_tx);
    const uint256 final_hash{MakeTransactionRef(final_tx)->GetHash()};

    server.SetTimedOutForTest();
    server.CheckTimeout();

    // Nobody misbehaved, so nobody may be charged and nothing may be reset: the timed-out but
    // fully signed session defers to the next CheckPool() round.
    BOOST_CHECK(server.consumed_collaterals.empty());
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});

    // That round commits it. The commit attempt is observable through the mempool prioritisation
    // CommitFinalTransaction() applies before submitting; the submission itself fails in this
    // fixture (the inputs do not exist), which resets the pool.
    server.CheckPoolForTest();
    CAmount delta{0};
    WITH_LOCK(m_node.mempool->cs, m_node.mempool->ApplyDelta(final_hash, delta));
    BOOST_CHECK_EQUAL(delta, static_cast<CAmount>(0.1 * COIN));
    BOOST_CHECK(server.consumed_collaterals.empty());
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
}

BOOST_AUTO_TEST_CASE(server_signing_saboteur_pays_instead_of_honest_participants)
{
    BOOST_REQUIRE(m_node.mn_sync);
    if (!m_node.mn_sync->IsBlockchainSynced()) m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
    auto& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);

    // A signing session with a saboteur that already signed and an honest participant that has
    // not yet. The saboteur resubmits an already-known signature, which fails AddScriptSig() and
    // makes the coordinator abort the session for everyone.
    const auto collateral_saboteur = MakeCollateral(0);
    const auto collateral_honest = MakeCollateral(1);

    auto saboteur = MakePeer(/*id=*/7, /*ipv4=*/0x0a000001);
    auto honest = MakePeer(/*id=*/8, /*ipv4=*/0x0a000002);
    saboteur->fSuccessfullyConnected = true;
    honest->fSuccessfullyConnected = true;

    server.ResetForTest(POOL_STATE_SIGNING);
    server.AddCollateralForTest(collateral_saboteur);
    server.AddCollateralForTest(collateral_honest);
    auto entry_saboteur = MakeEntry(collateral_saboteur, /*unsigned_inputs=*/0);
    entry_saboteur.addr = saboteur->addr;
    server.AddEntryForTest(entry_saboteur);
    auto entry_honest = MakeEntry(collateral_honest, /*unsigned_inputs=*/1);
    entry_honest.addr = honest->addr;
    server.AddEntryForTest(entry_honest);

    CNode* saboteur_node = saboteur.get();
    connman.AddTestNode(*saboteur.release());
    connman.AddTestNode(*honest.release());

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << std::vector<CTxIn>{CTxIn{COutPoint{uint256::ONE, 100}}};
    BOOST_CHECK_NO_THROW(server.ProcessMessage(*saboteur_node, NetMsgType::DSSIGNFINALTX, stream));

    // The abort charges the identifiable saboteur, immediately.
    BOOST_REQUIRE_EQUAL(server.consumed_collaterals.size(), 1U);
    BOOST_CHECK(*server.consumed_collaterals[0] == *collateral_saboteur);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});

    // At the timeout that follows, the honest participant's missing signature is the result of
    // obeying the coordinator's abort. It must not be treated as an offence.
    server.SetTimedOutForTest();
    server.CheckTimeout();
    BOOST_CHECK_EQUAL(server.consumed_collaterals.size(), 1U);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});

    connman.ClearTestNodes();
}

BOOST_AUTO_TEST_CASE(server_stale_signature_after_commit_does_not_poison_next_session)
{
    BOOST_REQUIRE(m_node.mn_sync);
    if (!m_node.mn_sync->IsBlockchainSynced()) m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    // While a DSSIGNFINALTX is being validated, a concurrent CheckPool() can commit the fully
    // signed session and reset the pool: committing does not wait for the in-flight mark. The
    // failure block must then recognize that the session it was admitted to has ended instead of
    // relaying an abort that would set m_relayed_abort on an idle pool - CreateNewSession() never
    // clears the flag, so the next session would inherit it and lose its guaranteed abort charge.
    class MidValidationResetServer : public TestableCoinJoinServer
    {
    public:
        using TestableCoinJoinServer::TestableCoinJoinServer;

        bool AddScriptSig(const CTxIn& txin) override
        {
            // The commit lands while this signature is validated; the pool is already reset by
            // the time the real AddScriptSig() runs, so it fails against an empty session.
            ClearPoolForTest();
            return RealAddScriptSig(txin);
        }
    };

    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    MidValidationResetServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                    *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                    *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                    *Assert(m_node.llmq_ctx->isman));

    const auto collateral = MakeCollateral(0);
    auto participant = MakePeer(/*id=*/7, /*ipv4=*/0x0a000001);
    server.ResetForTest(POOL_STATE_SIGNING);
    server.AddCollateralForTest(collateral);
    auto entry = MakeEntry(collateral, /*unsigned_inputs=*/0);
    entry.addr = participant->addr;
    server.AddEntryForTest(entry);

    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << std::vector<CTxIn>{CTxIn{COutPoint{uint256::ONE, 100}}};
    BOOST_CHECK_NO_THROW(server.ProcessMessage(*participant, NetMsgType::DSSIGNFINALTX, stream));

    // The stale failure charges nobody and leaves no abort mark behind.
    BOOST_CHECK(server.consumed_collaterals.empty());
    BOOST_CHECK(!server.RelayedAbortForTest());
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});

    // The next session - opened without a SetNull() in between, exactly like production - must
    // still be able to charge its own guaranteed abort fee.
    const auto next_collateral = MakeCollateral(1);
    PoolMessage message{MSG_NOERR};
    BOOST_REQUIRE(server.TryAdmit(next_collateral, message));
    server.EnterAcceptingEntriesState();
    server.SetTimedOutForTest();
    server.CheckTimeout();
    BOOST_REQUIRE_EQUAL(server.consumed_collaterals.size(), 1U);
    BOOST_CHECK(*server.consumed_collaterals[0] == *next_collateral);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
}

BOOST_AUTO_TEST_CASE(server_relayed_abort_forgoes_guaranteed_timeout_charge)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
    auto& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);

    // Two participants that have not signed yet; one is no longer connected. A session-wide
    // STATUS_REJECTED - as relayed when the final transaction cannot be delivered - tells the
    // connected one to stand down, so the timeout may not charge either of them: the abort was
    // the coordinator's, and a disconnect cannot be told apart from our own connection failing.
    const auto collateral_connected = MakeCollateral(0);
    const auto collateral_disconnected = MakeCollateral(1);

    auto connected = MakePeer(/*id=*/7, /*ipv4=*/0x0a000001);
    connected->fSuccessfullyConnected = true;

    server.ResetForTest(POOL_STATE_SIGNING);
    server.AddCollateralForTest(collateral_connected);
    server.AddCollateralForTest(collateral_disconnected);
    auto entry_connected = MakeEntry(collateral_connected, /*unsigned_inputs=*/1);
    entry_connected.addr = connected->addr;
    server.AddEntryForTest(entry_connected);
    auto entry_disconnected = MakeEntry(collateral_disconnected, /*unsigned_inputs=*/1);
    entry_disconnected.addr = MakePeer(/*id=*/8, /*ipv4=*/0x0a000002)->addr;
    server.AddEntryForTest(entry_disconnected);

    connman.AddTestNode(*connected.release());

    server.RelayAbortForTest();
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_SIGNING});

    server.SetTimedOutForTest();
    server.CheckTimeout();
    BOOST_CHECK(server.consumed_collaterals.empty());
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});

    connman.ClearTestNodes();
}

BOOST_AUTO_TEST_CASE(server_timeout_does_not_reset_during_pool_check)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
    server.SeedTimedOutSession();

    std::latch locked{1};
    std::latch release{1};
    std::thread pool_check{[&] { server.HoldPoolCheck(locked, release); }};
    locked.wait();

    server.CheckTimeout();
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_ACCEPTING_ENTRIES});

    release.count_down();
    pool_check.join();

    server.CheckTimeout();
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
}

BOOST_AUTO_TEST_CASE(server_timeout_does_not_reset_actionable_session)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));

    for (const auto state : {POOL_STATE_QUEUE, POOL_STATE_ACCEPTING_ENTRIES, POOL_STATE_SIGNING}) {
        BOOST_TEST_CONTEXT("state=" << state)
        {
            server.SeedTimedOutActionableSession(state);
            server.CheckTimeout();
            BOOST_CHECK_EQUAL(server.GetState(), int{state});
        }
    }

    server.SeedTimedOutActionableSession(POOL_STATE_ACCEPTING_ENTRIES, /*has_missing_entry=*/true);
    server.CheckTimeout();
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_ACCEPTING_ENTRIES});
}

BOOST_AUTO_TEST_CASE(server_validation_uses_session_denom_snapshot)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));

    const int session_denom{CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination())};
    const std::vector<CTxIn> vin{CTxIn{COutPoint{uint256::ONE, 0}}};
    const std::vector<CTxOut> vout{CTxOut{CoinJoin::GetSmallestDenomination(), P2PKHScript()}};
    PoolMessage message{MSG_NOERR};
    bool consume_collateral{false};

    // The live session denomination is zero, as it is after a concurrent SetNull(). Validation
    // must use the denomination captured before the reset instead of treating this as a punishable
    // denomination mismatch. The deliberately absent input makes validation stop with ERR_MISSING_TX.
    BOOST_CHECK(!server.ValidateInOuts(vin, vout, session_denom, message, consume_collateral));
    BOOST_CHECK_EQUAL(message, ERR_MISSING_TX);
    BOOST_CHECK(!consume_collateral);
}

BOOST_AUTO_TEST_CASE(entry_deserializes_vectors_through_wire_cap)
{
    const size_t wire_cap{CoinJoin::GetMaxPoolInputOutputCount()};
    BOOST_REQUIRE_GT(wire_cap, COINJOIN_ENTRY_MAX_SIZE);

    for (const size_t count : {size_t{0}, COINJOIN_ENTRY_MAX_SIZE, COINJOIN_ENTRY_MAX_SIZE + 1, wire_cap}) {
        BOOST_TEST_CONTEXT("count=" << count)
        {
            CCoinJoinEntry entry;
            entry.vecTxDSIn.resize(count);
            entry.vecTxOut.resize(count);

            CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
            stream << entry;

            CCoinJoinEntry roundtripped;
            BOOST_CHECK_NO_THROW(stream >> roundtripped);
            BOOST_CHECK_EQUAL(roundtripped.vecTxDSIn.size(), count);
            BOOST_CHECK_EQUAL(roundtripped.vecTxOut.size(), count);
        }
    }
}

BOOST_AUTO_TEST_CASE(entry_rejects_inputs_above_wire_cap_before_materializing)
{
    const size_t wire_cap{CoinJoin::GetMaxPoolInputOutputCount()};
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(stream, wire_cap + 1);

    CCoinJoinEntry entry;
    BOOST_CHECK_THROW(stream >> entry, std::ios_base::failure);
    BOOST_CHECK(entry.vecTxDSIn.empty());
}

BOOST_AUTO_TEST_CASE(entry_rejects_outputs_above_wire_cap_before_materializing)
{
    const size_t wire_cap{CoinJoin::GetMaxPoolInputOutputCount()};
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(stream, 0);
    stream << MakeTransactionRef(CMutableTransaction{});
    WriteCompactSize(stream, wire_cap + 1);

    CCoinJoinEntry entry;
    BOOST_CHECK_THROW(stream >> entry, std::ios_base::failure);
    BOOST_CHECK(entry.vecTxOut.empty());
}

BOOST_AUTO_TEST_CASE(queue_timeout_bounds)
{
    const auto now{std::chrono::time_point_cast<std::chrono::seconds>(GetAdjustedTime())};
    CCoinJoinQueue dsq{CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination()),
                       COutPoint{}, uint256::ONE, now, /*fReady=*/false};
    // current time -> not out of bounds
    BOOST_CHECK(!dsq.IsTimeOutOfBounds());

    // Too old (beyond COINJOIN_QUEUE_TIMEOUT)
    SetMockTime((now + std::chrono::seconds{COINJOIN_QUEUE_TIMEOUT + 1}).time_since_epoch());
    BOOST_CHECK(dsq.IsTimeOutOfBounds());

    // Too far in the future
    SetMockTime((now - std::chrono::seconds{COINJOIN_QUEUE_TIMEOUT + 1}).time_since_epoch());
    dsq.nTime = TicksSinceEpoch<std::chrono::seconds>(now + std::chrono::seconds{COINJOIN_QUEUE_TIMEOUT + 1});
    BOOST_CHECK(dsq.IsTimeOutOfBounds());

    // Reset mock time
    SetMockTime(0s);
}
BOOST_AUTO_TEST_SUITE_END()
