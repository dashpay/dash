// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/masternode.h>
#include <bls/bls.h>
#include <chain.h>
#include <coinjoin/coinjoin.h>
#include <coinjoin/common.h>
#include <coinjoin/server.h>
#include <evo/chainhelper.h>
#include <llmq/context.h>
#include <masternode/sync.h>
#include <net.h>
#include <node/connection_types.h>
#include <protocol.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/check.h>
#include <util/time.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
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

// Test-only subclass exposing the minimal seams needed to observe how
// ProcessDSSIGNFINALTX treats messages from participants vs. non-participants
// and to test offender collateral selection policies.
class TestableCoinJoinServer : public CCoinJoinServer
{
public:
    using CCoinJoinServer::CCoinJoinServer;

    mutable std::vector<CTransactionRef> vecConsumedCollaterals;

    void ConsumeCollateral(const CTransactionRef& txref) const override
    {
        vecConsumedCollaterals.push_back(txref);
    }

    void SetStateForTest(PoolState state)
    {
        nState = state;
    }

    void SetLastStepTimeForTest(int64_t nTime)
    {
        nTimeLastSuccessfulStep = nTime;
    }

    void AddSessionCollateral(const CTransactionRef& txCollateral)
    {
        LOCK(cs_coinjoin);
        vecSessionCollaterals.push_back(txCollateral);
    }

    void AddSessionEntry(const CCoinJoinEntry& entry)
    {
        LOCK(cs_coinjoin);
        vecEntries.push_back(entry);
    }

    CTransactionRef TestSelectCollateralToCharge(FeePolicy policy)
    {
        LOCK(cs_coinjoin);
        return SelectCollateralToCharge(policy);
    }

    void EnterSigningState() { nState = POOL_STATE_SIGNING; }

    void SeedParticipant(const CService& addr) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        CCoinJoinEntry entry;
        entry.addr = addr;
        LOCK(cs_coinjoin);
        vecEntries.push_back(std::move(entry));
    }
};

static CTransactionRef MakeMockCollateral(uint32_t id)
{
    CMutableTransaction mtx;
    mtx.vin.push_back(CTxIn(COutPoint(uint256::ONE, id)));
    mtx.vout.push_back(CTxOut(10000, P2PKHScript(static_cast<uint8_t>(id))));
    return MakeTransactionRef(mtx);
}

BOOST_AUTO_TEST_CASE(coinjoin_offender_selection_and_abort_fee_scenarios)
{
    BOOST_REQUIRE(m_node.mn_sync);
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));

    // 1. Queue timeout before readiness selects nothing
    {
        server.SetStateForTest(POOL_STATE_QUEUE);
        server.SetLastStepTimeForTest(GetTime() - COINJOIN_QUEUE_TIMEOUT - 10);
        server.CheckTimeout();
        BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
        BOOST_CHECK(server.vecConsumedCollaterals.empty());
    }

    // 2. Twenty reservations and zero entries select exactly one collateral under GUARANTEED_ON_ABORT
    {
        TestableCoinJoinServer s2(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
        s2.SetStateForTest(POOL_STATE_ACCEPTING_ENTRIES);
        for (uint32_t i = 0; i < 20; ++i) {
            s2.AddSessionCollateral(MakeMockCollateral(i));
        }
        CTransactionRef selected = s2.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
        BOOST_CHECK(selected != nullptr);

        // PROBABILISTIC returns nullptr because all participants are offenders (all-offenders exemption)
        CTransactionRef prob_selected = s2.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::PROBABILISTIC);
        BOOST_CHECK(prob_selected == nullptr);
    }

    // 3. Twenty reservations and one entry select one of the nineteen missing participants
    {
        TestableCoinJoinServer s3(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
        s3.SetStateForTest(POOL_STATE_ACCEPTING_ENTRIES);
        std::vector<CTransactionRef> collaterals;
        for (uint32_t i = 0; i < 20; ++i) {
            auto col = MakeMockCollateral(i);
            collaterals.push_back(col);
            s3.AddSessionCollateral(col);
        }
        // Add 1 entry matching collateral 0
        CCoinJoinEntry entry0({}, {}, CTransaction(*collaterals[0]));
        s3.AddSessionEntry(entry0);

        for (int run = 0; run < 50; ++run) {
            CTransactionRef selected = s3.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
            BOOST_REQUIRE(selected != nullptr);
            BOOST_CHECK(*selected != *collaterals[0]);
        }
    }

    // 4. Five reservations and three entries select only from the two missing participants
    {
        TestableCoinJoinServer s4(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
        s4.SetStateForTest(POOL_STATE_ACCEPTING_ENTRIES);
        std::vector<CTransactionRef> collaterals;
        for (uint32_t i = 0; i < 5; ++i) {
            auto col = MakeMockCollateral(i);
            collaterals.push_back(col);
            s4.AddSessionCollateral(col);
        }
        // Entries for 0, 1, 2
        for (uint32_t i = 0; i < 3; ++i) {
            CCoinJoinEntry entry({}, {}, CTransaction(*collaterals[i]));
            s4.AddSessionEntry(entry);
        }

        for (int run = 0; run < 50; ++run) {
            CTransactionRef selected = s4.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
            BOOST_REQUIRE(selected != nullptr);
            BOOST_CHECK(*selected == *collaterals[3] || *selected == *collaterals[4]);
        }
    }

    // 5. Every reservation has an entry: no missing-entry collateral is selected
    {
        TestableCoinJoinServer s5(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
        s5.SetStateForTest(POOL_STATE_ACCEPTING_ENTRIES);
        for (uint32_t i = 0; i < 3; ++i) {
            auto col = MakeMockCollateral(i);
            s5.AddSessionCollateral(col);
            CCoinJoinEntry entry({}, {}, CTransaction(*col));
            s5.AddSessionEntry(entry);
        }
        BOOST_CHECK(s5.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT) == nullptr);
        BOOST_CHECK(s5.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::PROBABILISTIC) == nullptr);
    }

    // 6. One non-signer is selected deterministically
    {
        TestableCoinJoinServer s6(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
        s6.SetStateForTest(POOL_STATE_SIGNING);
        std::vector<CTransactionRef> collaterals;
        for (uint32_t i = 0; i < 3; ++i) {
            auto col = MakeMockCollateral(i);
            collaterals.push_back(col);
            s6.AddSessionCollateral(col);
        }

        // Entries 0 and 1 have signed inputs, entry 2 has an unsigned input
        CTxDSIn dsin_signed(CTxIn(COutPoint(uint256::ONE, 100)), P2PKHScript(1), 0);
        dsin_signed.fHasSig = true;
        CTxDSIn dsin_unsigned(CTxIn(COutPoint(uint256::ONE, 101)), P2PKHScript(2), 0);
        dsin_unsigned.fHasSig = false;

        CCoinJoinEntry e0({dsin_signed}, {}, CTransaction(*collaterals[0]));
        CCoinJoinEntry e1({dsin_signed}, {}, CTransaction(*collaterals[1]));
        CCoinJoinEntry e2({dsin_unsigned}, {}, CTransaction(*collaterals[2]));

        s6.AddSessionEntry(e0);
        s6.AddSessionEntry(e1);
        s6.AddSessionEntry(e2);

        CTransactionRef selected = s6.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
        BOOST_REQUIRE(selected != nullptr);
        BOOST_CHECK(*selected == *collaterals[2]);
    }

    // 7. Multiple non-signers produce one selection from that set
    {
        TestableCoinJoinServer s7(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
        s7.SetStateForTest(POOL_STATE_SIGNING);
        std::vector<CTransactionRef> collaterals;
        for (uint32_t i = 0; i < 3; ++i) {
            auto col = MakeMockCollateral(i);
            collaterals.push_back(col);
            s7.AddSessionCollateral(col);
        }

        CTxDSIn dsin_signed(CTxIn(COutPoint(uint256::ONE, 100)), P2PKHScript(1), 0);
        dsin_signed.fHasSig = true;
        CTxDSIn dsin_unsigned(CTxIn(COutPoint(uint256::ONE, 101)), P2PKHScript(2), 0);
        dsin_unsigned.fHasSig = false;

        CCoinJoinEntry e0({dsin_signed}, {}, CTransaction(*collaterals[0]));
        CCoinJoinEntry e1({dsin_unsigned}, {}, CTransaction(*collaterals[1]));
        CCoinJoinEntry e2({dsin_unsigned}, {}, CTransaction(*collaterals[2]));

        s7.AddSessionEntry(e0);
        s7.AddSessionEntry(e1);
        s7.AddSessionEntry(e2);

        for (int run = 0; run < 50; ++run) {
            CTransactionRef selected = s7.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
            BOOST_REQUIRE(selected != nullptr);
            BOOST_CHECK(*selected == *collaterals[1] || *selected == *collaterals[2]);
        }
    }

    // 8. Participant with several unsigned inputs appears only once in candidate set
    {
        TestableCoinJoinServer s8(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
        s8.SetStateForTest(POOL_STATE_SIGNING);
        auto c0 = MakeMockCollateral(0);
        auto c1 = MakeMockCollateral(1);
        s8.AddSessionCollateral(c0);
        s8.AddSessionCollateral(c1);

        CTxDSIn u1(CTxIn(COutPoint(uint256::ONE, 10)), P2PKHScript(1), 0); u1.fHasSig = false;
        CTxDSIn u2(CTxIn(COutPoint(uint256::ONE, 11)), P2PKHScript(1), 0); u2.fHasSig = false;
        CTxDSIn u3(CTxIn(COutPoint(uint256::ONE, 12)), P2PKHScript(1), 0); u3.fHasSig = false;
        CTxDSIn u4(CTxIn(COutPoint(uint256::ONE, 20)), P2PKHScript(2), 0); u4.fHasSig = false;

        // Entry 0 has 3 unsigned inputs, Entry 1 has 1 unsigned input
        CCoinJoinEntry e0({u1, u2, u3}, {}, CTransaction(*c0));
        CCoinJoinEntry e1({u4}, {}, CTransaction(*c1));

        s8.AddSessionEntry(e0);
        s8.AddSessionEntry(e1);

        int count_c0 = 0;
        int count_c1 = 0;
        const int total_runs = 1000;
        for (int run = 0; run < total_runs; ++run) {
            CTransactionRef selected = s8.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
            BOOST_REQUIRE(selected != nullptr);
            if (*selected == *c0) count_c0++;
            else if (*selected == *c1) count_c1++;
        }
        // With deduplication, both have equal weight (50% each), so each should be ~400-600 out of 1000
        BOOST_CHECK_GT(count_c0, 350);
        BOOST_CHECK_GT(count_c1, 350);
    }

    // 9. Every participant fails to sign: exactly one participant selected under GUARANTEED_ON_ABORT
    {
        TestableCoinJoinServer s9(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.llmq_ctx->isman));
        s9.SetStateForTest(POOL_STATE_SIGNING);
        for (uint32_t i = 0; i < 3; ++i) {
            auto col = MakeMockCollateral(i);
            s9.AddSessionCollateral(col);
            CTxDSIn u(CTxIn(COutPoint(uint256::ONE, i + 10)), P2PKHScript(1), 0); u.fHasSig = false;
            CCoinJoinEntry e({u}, {}, CTransaction(*col));
            s9.AddSessionEntry(e);
        }
        BOOST_CHECK(s9.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::PROBABILISTIC) == nullptr);

        CTransactionRef selected = s9.TestSelectCollateralToCharge(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
        BOOST_CHECK(selected != nullptr);
    }

    // 10. A session reset during timeout handling cannot charge or mutate the following session
    {
        TestableCoinJoinServer s10(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                   *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                   *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                   *Assert(m_node.llmq_ctx->isman));
        s10.SetStateForTest(POOL_STATE_ACCEPTING_ENTRIES);
        s10.SetLastStepTimeForTest(GetTime() - COINJOIN_QUEUE_TIMEOUT - 10);
        auto col = MakeMockCollateral(1);
        s10.AddSessionCollateral(col);

        s10.CheckTimeout();
        BOOST_CHECK_EQUAL(s10.GetState(), int{POOL_STATE_IDLE});
        BOOST_CHECK_EQUAL(s10.vecConsumedCollaterals.size(), 1U);
        BOOST_CHECK(*s10.vecConsumedCollaterals[0] == *col);

        // Second CheckTimeout call on idle session does nothing
        s10.CheckTimeout();
        BOOST_CHECK_EQUAL(s10.vecConsumedCollaterals.size(), 1U);
    }

    // 11. Recoverable timeouts retain probabilistic policy (tested implicitly via CheckPool logic)
    // 12. Successful-session random charging remains unchanged
    // 13. State is POOL_STATE_ERROR during collateral consumption before SetNull resets to POOL_STATE_IDLE
    {
        class StateCheckingServer : public TestableCoinJoinServer
        {
        public:
            using TestableCoinJoinServer::TestableCoinJoinServer;
            mutable int state_during_consume{POOL_STATE_IDLE};

            void ConsumeCollateral(const CTransactionRef& txref) const override
            {
                state_during_consume = GetState();
                TestableCoinJoinServer::ConsumeCollateral(txref);
            }
        };

        StateCheckingServer s13(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                *Assert(m_node.llmq_ctx->isman));
        s13.SetStateForTest(POOL_STATE_ACCEPTING_ENTRIES);
        s13.SetLastStepTimeForTest(GetTime() - COINJOIN_QUEUE_TIMEOUT - 10);
        auto col = MakeMockCollateral(1);
        s13.AddSessionCollateral(col);

        s13.CheckTimeout();
        BOOST_CHECK_EQUAL(s13.state_during_consume, int{POOL_STATE_ERROR});
        BOOST_CHECK_EQUAL(s13.GetState(), int{POOL_STATE_IDLE});
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
