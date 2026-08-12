// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/masternode.h>
#include <bls/bls.h>
#include <chain.h>
#include <chainlock/chainlock.h>
#include <coinjoin/coinjoin.h>
#include <coinjoin/common.h>
#include <coinjoin/options.h>
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
    // Pre-V24 behavior (nullptr pindex = pre-fork)
    BOOST_CHECK(good.IsValidStructure(nullptr, *Assert(m_node.chainman)));

    // Bad: both identifiers null
    CCoinJoinBroadcastTx bad_ids = good;
    bad_ids.m_protxHash = uint256{};
    bad_ids.masternodeOutpoint.SetNull();
    BOOST_CHECK(!bad_ids.IsValidStructure(nullptr, *Assert(m_node.chainman)));

    // Bad: vin/vout size mismatch (invalid pre-V24)
    CCoinJoinBroadcastTx bad_sizes = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout.pop_back();
        bad_sizes.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_sizes.IsValidStructure(nullptr, *Assert(m_node.chainman)));

    // Bad: non-P2PKH output
    CCoinJoinBroadcastTx bad_script = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout[0].scriptPubKey = CScript() << OP_RETURN << std::vector<unsigned char>{'x'};
        bad_script.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_script.IsValidStructure(nullptr, *Assert(m_node.chainman)));

    // Bad: non-denominated amount
    CCoinJoinBroadcastTx bad_amount = good;
    {
        CMutableTransaction mtx(*good.tx);
        mtx.vout[0].nValue = 42; // not a valid denom
        bad_amount.tx = MakeTransactionRef(mtx);
    }
    BOOST_CHECK(!bad_amount.IsValidStructure(nullptr, *Assert(m_node.chainman)));
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
    using CCoinJoinServer::AddEntry;
    using CCoinJoinServer::CCoinJoinServer;
    using CCoinJoinServer::CreateFinalTransaction;

    // A live session always carries a non-zero id, and AddEntry rejects entries that don't
    // belong to one, so seed an id along with the state. Entering a state also starts its
    // timeout, as SetState() does, so the session is not seeded as already timed out.
    void EnterSigningState()
    {
        nSessionID = 1;
        nState = POOL_STATE_SIGNING;
        nTimeLastSuccessfulStep = GetTime();
    }
    void EnterAcceptingEntriesState()
    {
        nSessionID = 1;
        nState = POOL_STATE_ACCEPTING_ENTRIES;
        nTimeLastSuccessfulStep = GetTime();
    }

    void SetFinalTransactionForTest(const CMutableTransaction& tx)
    {
        LOCK(cs_coinjoin);
        finalMutableTransaction = tx;
    }

    void CheckPoolForTest() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin, !cs_check_pool) { CheckPool(); }

    void SeedParticipant(const CService& addr) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        CCoinJoinEntry entry;
        entry.addr = addr;
        LOCK(cs_coinjoin);
        vecEntries.push_back(std::move(entry));
    }

    void SeedSessionCollateral(const CMutableTransaction& txCollateral, CoinJoin::MixShape shape)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        m_mapDeclaredShapes.emplace(txCollateral.GetHash(), shape);
        m_session_collaterals.Add(txCollateral);
    }

    void SeedEntry(CCoinJoinEntry entry) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        vecEntries.push_back(std::move(entry));
    }

    void SeedCompletionSession(int session_id, const CService& addr, PoolState state = POOL_STATE_SIGNING)
        EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
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

    void RelayCompletion(int session_id, const CService& addr) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        RelayCompletedTransaction(session_id, {addr}, MSG_SUCCESS);
    }

    void ResetForSession(int session_id) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        ResetSigningSessionIfCurrent(session_id);
    }

    void SeedTimedOutSession() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
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
        EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
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
            m_mapDeclaredShapes.emplace(collateral.GetHash(), CoinJoin::MixShape::STANDARD);
            m_session_collaterals.Add(collateral);

            if (state == POOL_STATE_QUEUE) continue;

            CTxDSIn txdsin{CTxIn{COutPoint{uint256::TWO, static_cast<uint32_t>(i)}}, P2PKHScript(), 0};
            txdsin.fHasSig = state == POOL_STATE_SIGNING;
            std::vector<CTxOut> outputs{CTxOut{CoinJoin::GetSmallestDenomination(), P2PKHScript()}};
            vecEntries.emplace_back(std::vector<CTxDSIn>{txdsin}, std::move(outputs), CTransaction{collateral});
        }

        if (has_missing_entry) {
            CMutableTransaction collateral;
            collateral.vin.emplace_back(COutPoint{uint256::ONE, static_cast<uint32_t>(CoinJoin::GetMinPoolParticipants())});
            m_mapDeclaredShapes.emplace(collateral.GetHash(), CoinJoin::MixShape::STANDARD);
            m_session_collaterals.Add(collateral);
        }
    }

    void HoldPoolCheck(std::latch& locked, std::latch& release) EXCLUSIVE_LOCKS_REQUIRED(!cs_check_pool)
    {
        LOCK(cs_check_pool);
        locked.count_down();
        release.wait();
    }

    void ResetForTest(PoolState state) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        SetNull();
        nState = state;
        nSessionID = 1;
        nTimeLastSuccessfulStep = GetTime();
    }

    void SetTimedOutForTest() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        nTimeLastSuccessfulStep = GetTime() -
                                  ((nState == POOL_STATE_SIGNING) ? COINJOIN_SIGNING_TIMEOUT : COINJOIN_QUEUE_TIMEOUT);
    }

    void AddCollateralForTest(const CTransactionRef& collateral) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        m_session_collaterals.Add(CMutableTransaction{*collateral});
    }

    CTransactionRef SelectForTest(FeePolicy policy) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        return SelectCollateralToCharge(policy);
    }

    void ChargeRandomFeesForTest() const EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        ChargeRandomFees(WITH_LOCK(cs_coinjoin, return m_session_collaterals.txs()));
    }

    //! Offer a collateral to CreateNewSession() the way a DSACCEPT would. fUnitTest skips the
    //! mempool-backed collateral validity check and the dsq relay, isolating admission logic.
    bool TryAdmit(const CTransactionRef& collateral, PoolMessage& message) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        fUnitTest = true;
        const CCoinJoinAccept dsa{CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination()),
                                  CMutableTransaction{*collateral}};
        return CreateNewSession(dsa, PROTOCOL_VERSION, message);
    }

    int MarkMessageInFlightForTest() EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        LOCK(cs_coinjoin);
        return MarkMessageInFlight();
    }

    void ClearMessageInFlightForTest(int session_id) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        ClearMessageInFlight(session_id);
    }
};

//! Records the collaterals the server charges instead of submitting them to the mempool. Every
//! test drives the server from its own thread only, so the record needs no synchronization.
class ChargeRecordingServer : public TestableCoinJoinServer
{
public:
    using TestableCoinJoinServer::TestableCoinJoinServer;

    mutable std::vector<CTransactionRef> consumed_collaterals;

    void ConsumeCollateral(const CTransactionRef& txref) const override EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        consumed_collaterals.push_back(txref);
    }

    void ResetForTest(PoolState state) EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
    {
        TestableCoinJoinServer::ResetForTest(state);
        consumed_collaterals.clear();
    }
};

//! A server, and the masternode manager it borrows, wired to the test node.
template <typename Server = ChargeRecordingServer>
struct ServerHarness {
    CActiveMasternodeManager mn_activeman;
    Server server;

    explicit ServerHarness(node::NodeContext& node) :
        mn_activeman{*Assert(node.connman), *Assert(node.dmnman), MakeSecretKey()},
        server{node.peerman.get(), *Assert(node.chainman), *Assert(node.connman), *Assert(node.dmnman),
               *Assert(node.dstxman), *Assert(node.mn_metaman), *Assert(node.mempool), mn_activeman,
               *Assert(node.mn_sync), *Assert(node.isman)}
    {
    }
};

static CTransactionRef MakeCollateral(uint32_t id)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{uint256::ONE, id});
    tx.vout.emplace_back(CoinJoin::GetCollateralAmount(), P2PKHScript(static_cast<uint8_t>(id)));
    return MakeTransactionRef(tx);
}

//! An entry for collateral. unsigned_inputs == 0 gives one signed input (a participant that has
//! signed, or merely submitted); unsigned_inputs == n > 0 gives n inputs that are still unsigned.
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
                                  *Assert(m_node.isman));

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
                                  *Assert(m_node.isman));

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

//! Re-export the protected static validation helper so it can be called
//! directly, without standing up a CCoinJoinServer.
struct InOutsChecker : CCoinJoinBaseSession
{
    using CCoinJoinBaseSession::IsValidInOuts;
};

BOOST_AUTO_TEST_CASE(validation_uses_session_denom_snapshot)
{
    Chainstate& chainstate{Assert(m_node.chainman)->ActiveChainstate()};
    const auto& isman{*Assert(m_node.isman)};
    const auto& mempool{*Assert(m_node.mempool)};

    const int session_denom{CoinJoin::AmountToDenomination(CoinJoin::GetSmallestDenomination())};
    const std::vector<CTxIn> vin{CTxIn{COutPoint{uint256::ONE, 0}}};
    const std::vector<CTxOut> vout{CTxOut{CoinJoin::GetSmallestDenomination(), P2PKHScript()}};
    PoolMessage message{MSG_NOERR};
    bool consume_collateral{false};

    // Outputs matching the captured denomination pass the denom check and fail
    // only later on the unknown input.
    BOOST_CHECK(!InOutsChecker::IsValidInOuts(chainstate, isman, mempool, vin, vout, session_denom,
                                              /*fAllowRebalanceShapes=*/false, message, &consume_collateral));
    BOOST_CHECK_EQUAL(message, ERR_MISSING_TX);
    BOOST_CHECK(!consume_collateral);

    // A mismatched captured denomination is rejected up front and flags the
    // entry's collateral for consumption.
    const int other_denom{CoinJoin::AmountToDenomination(CoinJoin::GetStandardDenominations().front())};
    BOOST_REQUIRE(other_denom != session_denom);
    BOOST_CHECK(!InOutsChecker::IsValidInOuts(chainstate, isman, mempool, vin, vout, other_denom,
                                              /*fAllowRebalanceShapes=*/false, message, &consume_collateral));
    BOOST_CHECK_EQUAL(message, ERR_DENOM);
    BOOST_CHECK(consume_collateral);
}

BOOST_AUTO_TEST_CASE(server_addentry_binds_entries_to_accepted_collaterals)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.isman));

    auto make_collateral = [](uint8_t tag) {
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint(uint256::ONE, tag));
        tx.vout.emplace_back(COIN / 10, P2PKHScript(tag));
        return tx;
    };
    // Distinct mixing inputs per entry so none of the checks below trip the duplicate-txin path.
    auto make_entry = [](const CMutableTransaction& txCollateral, uint8_t tag) {
        std::vector<CTxDSIn> dsins{CTxDSIn(CTxIn(COutPoint(uint256S("aa"), tag)), P2PKHScript(tag), /*nRounds=*/0)};
        std::vector<CTxOut> outs{CTxOut(COIN / 1000 + 1, P2PKHScript(tag))};
        return CCoinJoinEntry(dsins, outs, CTransaction(txCollateral));
    };

    const CMutableTransaction collateral_a = make_collateral(1);
    const CMutableTransaction collateral_b = make_collateral(2);
    const CMutableTransaction collateral_unadmitted = make_collateral(3);

    server.SeedSessionCollateral(collateral_a, CoinJoin::MixShape::STANDARD);
    server.SeedSessionCollateral(collateral_b, CoinJoin::MixShape::STANDARD);
    server.EnterAcceptingEntriesState();

    // A collateral that never went through dsa acceptance cannot submit an entry, even
    // though the session still has free slots.
    PoolMessage msg{MSG_NOERR};
    BOOST_CHECK(!server.AddEntry(make_entry(collateral_unadmitted, 0x10), msg));
    BOOST_CHECK_EQUAL(msg, ERR_SESSION);
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 0);

    // An accepted collateral covers exactly one entry: a second entry reusing the
    // collateral of an existing entry is rejected.
    server.SeedEntry(make_entry(collateral_a, 0x11));
    msg = MSG_NOERR;
    BOOST_CHECK(!server.AddEntry(make_entry(collateral_a, 0x12), msg));
    BOOST_CHECK_EQUAL(msg, ERR_ALREADY_HAVE);
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 1);

    // An accepted, unused collateral passes both checks and proceeds to collateral
    // validation (which fails here because the fake collateral has no UTXO backing).
    msg = MSG_NOERR;
    BOOST_CHECK(!server.AddEntry(make_entry(collateral_b, 0x13), msg));
    BOOST_CHECK_EQUAL(msg, ERR_INVALID_COLLATERAL);
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 1);
}

BOOST_AUTO_TEST_CASE(server_addentry_rejects_entries_once_the_session_finalized)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.isman));

    CMutableTransaction txCollateral;
    txCollateral.vin.emplace_back(COutPoint(uint256::ONE, 1));
    txCollateral.vout.emplace_back(COIN / 10, P2PKHScript(1));

    std::vector<CTxDSIn> dsins{CTxDSIn(CTxIn(COutPoint(uint256S("aa"), 1)), P2PKHScript(1), /*nRounds=*/0)};
    std::vector<CTxOut> outs{CTxOut(COIN / 1000 + 1, P2PKHScript(1))};
    const CCoinJoinEntry entry(dsins, outs, CTransaction(txCollateral));

    server.SeedSessionCollateral(txCollateral, CoinJoin::MixShape::STANDARD);

    // The scheduler thread can finalize a timed-out session while a straggler's entry is still
    // being validated. Its collateral is still committed and it still has no entry, so the
    // membership and duplicate checks alone would let it in - after the final transaction was
    // already built without it, leaving inputs nobody can sign.
    server.EnterSigningState();

    PoolMessage msg{MSG_NOERR};
    BOOST_CHECK(!server.AddEntry(entry, msg));
    BOOST_CHECK_EQUAL(msg, ERR_SESSION);
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 0);
}

BOOST_AUTO_TEST_CASE(server_finalization_rechecks_live_side_coverage)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.isman));

    const auto make_entry = [](CoinJoin::MixShape shape, uint32_t tag) {
        const size_t input_count{shape == CoinJoin::MixShape::PROMOTION ? size_t{CoinJoin::PROMOTION_RATIO} : 1};
        const size_t output_count{shape == CoinJoin::MixShape::DEMOTION ? size_t{CoinJoin::PROMOTION_RATIO} : 1};
        std::vector<CTxDSIn> inputs;
        std::vector<CTxOut> outputs;
        for (size_t i{0}; i < input_count; ++i) {
            inputs.emplace_back(CTxIn{COutPoint{uint256::ONE, tag + static_cast<uint32_t>(i)}}, P2PKHScript(), 0);
        }
        for (size_t i{0}; i < output_count; ++i) {
            outputs.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(tag + i)));
        }
        return CCoinJoinEntry{inputs, outputs, CTransaction{CMutableTransaction{}}};
    };

    server.SeedEntry(make_entry(CoinJoin::MixShape::DEMOTION, 0));
    server.SeedEntry(make_entry(CoinJoin::MixShape::DEMOTION, 10));
    server.SeedEntry(make_entry(CoinJoin::MixShape::DEMOTION, 20));
    server.SeedEntry(make_entry(CoinJoin::MixShape::PROMOTION, 30));
    server.EnterAcceptingEntriesState();

    // A timeout snapshot could have observed only the three demotions as covered (0/3), then
    // this first promotion could commit before finalization took the lock. Finalization must use
    // the live 1/3 side counts and refuse to build the uncovered transaction, staying out of
    // POOL_STATE_SIGNING; the still-timed-out session is then reset by the scheduler's
    // regular CheckTimeout() pass instead of leaking a lone promoter on-chain.
    server.CreateFinalTransaction(/*session_id=*/1, /*charge_fees=*/true);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_ACCEPTING_ENTRIES});
    BOOST_CHECK_EQUAL(server.GetEntriesCount(), 4);
}

BOOST_AUTO_TEST_CASE(server_completion_does_not_reset_an_unreachable_or_replacement_session)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.isman));

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

    // The signing session it was issued for is still reset.
    server.SeedCompletionSession(/*session_id=*/1, participant->addr);
    server.ResetForSession(/*session_id=*/1);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
}

BOOST_AUTO_TEST_CASE(server_timeout_defers_and_commits_fully_signed_session)
{
    ServerHarness harness{m_node};
    auto& server{harness.server};

    // A fully signed session whose committing CheckPool() round was skipped: the final signature
    // arrived while the scheduler held cs_check_pool, so its DSSIGNFINALTX could not commit, and
    // the scheduler's own sample predated the signature. CheckTimeout() then runs past the
    // deadline and must not treat the session as failed.
    const auto collateral = MakeCollateral(0);
    server.ResetForTest(POOL_STATE_SIGNING);
    server.AddCollateralForTest(collateral);
    server.SeedEntry(MakeEntry(collateral, /*unsigned_inputs=*/0));

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
    BOOST_CHECK_EQUAL(delta, COIN / 10);
    BOOST_CHECK(server.consumed_collaterals.empty());
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
}

BOOST_AUTO_TEST_CASE(server_timeout_does_not_reset_during_pool_check)
{
    CActiveMasternodeManager mn_activeman(*Assert(m_node.connman), *Assert(m_node.dmnman), MakeSecretKey());
    TestableCoinJoinServer server(m_node.peerman.get(), *Assert(m_node.chainman), *Assert(m_node.connman),
                                  *Assert(m_node.dmnman), *Assert(m_node.dstxman), *Assert(m_node.mn_metaman),
                                  *Assert(m_node.mempool), mn_activeman, *Assert(m_node.mn_sync),
                                  *Assert(m_node.isman));
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
                                  *Assert(m_node.isman));

    // A queue that just became ready, an entry for every collateral, and a fully signed final
    // transaction can all still advance, so a timeout landing after the scheduler's own
    // CheckPool() snapshot must leave them for the next tick instead of discarding them.
    for (const auto state : {POOL_STATE_QUEUE, POOL_STATE_ACCEPTING_ENTRIES, POOL_STATE_SIGNING}) {
        BOOST_TEST_CONTEXT("state=" << state)
        {
            server.SeedTimedOutActionableSession(state);
            server.CheckTimeout();
            BOOST_CHECK_EQUAL(server.GetState(), int{state});
        }
    }

    // Enough covered entries to finalize without the straggler.
    server.SeedTimedOutActionableSession(POOL_STATE_ACCEPTING_ENTRIES, /*has_missing_entry=*/true);
    server.CheckTimeout();
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_ACCEPTING_ENTRIES});
}

BOOST_AUTO_TEST_CASE(server_queue_timeout_charges_nobody)
{
    ServerHarness harness{m_node};
    auto& server{harness.server};

    // Nobody was asked to submit anything before the queue became ready, so a queue that never
    // did has no identifiable offender.
    server.ResetForTest(POOL_STATE_QUEUE);
    server.AddCollateralForTest(MakeCollateral(0));
    server.SetTimedOutForTest();
    server.CheckTimeout();
    BOOST_CHECK(server.consumed_collaterals.empty());
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
}

BOOST_AUTO_TEST_CASE(server_abort_charges_one_collateral_when_every_reservation_failed)
{
    ServerHarness harness{m_node};
    auto& server{harness.server};

    // All 20 reservations failed to submit. The probabilistic policy exempts a session in which
    // everyone offended; the abort policy still selects exactly one of them.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (uint32_t i = 0; i < 20; ++i) {
        server.AddCollateralForTest(MakeCollateral(i));
    }
    BOOST_CHECK(server.SelectForTest(CCoinJoinServer::FeePolicy::PROBABILISTIC) == nullptr);
    BOOST_CHECK(server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT) != nullptr);

    server.SetTimedOutForTest();
    server.CheckTimeout();
    BOOST_CHECK_EQUAL(server.consumed_collaterals.size(), 1U);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
}

BOOST_AUTO_TEST_CASE(server_abort_selects_only_missing_submitters)
{
    ServerHarness harness{m_node};
    auto& server{harness.server};
    std::vector<CTransactionRef> collaterals;
    for (uint32_t i = 0; i < 20; ++i) {
        collaterals.push_back(MakeCollateral(i));
    }

    // With one submission, only one of the other 19 reservations can be selected.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (const auto& collateral : collaterals) server.AddCollateralForTest(collateral);
    server.SeedEntry(MakeEntry(collaterals[0]));
    for (int i = 0; i < 64; ++i) {
        const auto selected = server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
        BOOST_REQUIRE(selected);
        BOOST_CHECK(*selected != *collaterals[0]);
    }

    // Three of five submitted, so only the two missing reservations are eligible.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (size_t i = 0; i < 5; ++i) server.AddCollateralForTest(collaterals[i]);
    for (size_t i = 0; i < 3; ++i) server.SeedEntry(MakeEntry(collaterals[i]));
    for (int i = 0; i < 64; ++i) {
        const auto selected = server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
        BOOST_REQUIRE(selected);
        BOOST_CHECK(*selected == *collaterals[3] || *selected == *collaterals[4]);
    }

    // If every reservation submitted, there is no missing-entry collateral to select.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (size_t i = 0; i < 5; ++i) {
        server.AddCollateralForTest(collaterals[i]);
        server.SeedEntry(MakeEntry(collaterals[i]));
    }
    BOOST_CHECK(server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT) == nullptr);
}

BOOST_AUTO_TEST_CASE(server_abort_selects_each_non_signer_once)
{
    ServerHarness harness{m_node};
    auto& server{harness.server};
    std::vector<CTransactionRef> collaterals;
    for (uint32_t i = 0; i < 3; ++i) {
        collaterals.push_back(MakeCollateral(i));
    }

    // A lone non-signer is selected deterministically.
    server.ResetForTest(POOL_STATE_SIGNING);
    for (size_t i = 0; i < 3; ++i) {
        server.AddCollateralForTest(collaterals[i]);
        server.SeedEntry(MakeEntry(collaterals[i], i == 2 ? 1 : 0));
    }
    const auto lone_non_signer = server.SelectForTest(CCoinJoinServer::FeePolicy::GUARANTEED_ON_ABORT);
    BOOST_REQUIRE(lone_non_signer);
    BOOST_CHECK(*lone_non_signer == *collaterals[2]);

    // Multiple non-signers select one member of that set; the number of unsigned inputs gives a
    // participant no extra weight.
    server.ResetForTest(POOL_STATE_SIGNING);
    server.AddCollateralForTest(collaterals[0]);
    server.SeedEntry(MakeEntry(collaterals[0], /*unsigned_inputs=*/8));
    server.AddCollateralForTest(collaterals[1]);
    server.SeedEntry(MakeEntry(collaterals[1], /*unsigned_inputs=*/1));
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

    // When every participant fails to sign, the abort still charges exactly one.
    server.SetTimedOutForTest();
    server.CheckTimeout();
    BOOST_CHECK_EQUAL(server.consumed_collaterals.size(), 1U);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_IDLE});
}

BOOST_AUTO_TEST_CASE(server_recoverable_timeout_keeps_probabilistic_policy)
{
    ServerHarness harness{m_node};
    auto& server{harness.server};

    // Three of five submitted: enough to finalize without the stragglers, which keeps the
    // historical charge-sometimes policy rather than the abort guarantee.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (uint32_t i = 0; i < 5; ++i) {
        const auto collateral{MakeCollateral(i)};
        server.AddCollateralForTest(collateral);
        if (i < 3) server.SeedEntry(MakeEntry(collateral));
    }
    bool selected{false};
    bool skipped{false};
    for (int i = 0; i < 256; ++i) {
        if (server.SelectForTest(CCoinJoinServer::FeePolicy::PROBABILISTIC)) {
            selected = true;
        } else {
            skipped = true;
        }
    }
    BOOST_CHECK(selected);
    BOOST_CHECK(skipped);
}

BOOST_AUTO_TEST_CASE(server_successful_session_random_charge_stays_probabilistic)
{
    ServerHarness harness{m_node};
    auto& server{harness.server};

    server.ResetForTest(POOL_STATE_SIGNING);
    for (uint32_t i = 0; i < 5; ++i) {
        server.AddCollateralForTest(MakeCollateral(i));
    }
    bool charged{false};
    bool skipped{false};
    for (int i = 0; i < 256; ++i) {
        server.consumed_collaterals.clear();
        server.ChargeRandomFeesForTest();
        charged |= !server.consumed_collaterals.empty();
        skipped |= server.consumed_collaterals.empty();
    }
    BOOST_CHECK(charged);
    BOOST_CHECK(skipped);
}

BOOST_AUTO_TEST_CASE(server_timeout_reset_precedes_collateral_consumption)
{
    class SessionReplacingServer : public ChargeRecordingServer
    {
    public:
        using ChargeRecordingServer::ChargeRecordingServer;
        mutable PoolState state_during_consume{POOL_STATE_ERROR};

        void ConsumeCollateral(const CTransactionRef& txref) const override EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
        {
            state_during_consume = static_cast<PoolState>(GetState());
            auto* self = const_cast<SessionReplacingServer*>(this);
            self->TestableCoinJoinServer::ResetForTest(POOL_STATE_QUEUE);
            ChargeRecordingServer::ConsumeCollateral(txref);
        }
    };

    ServerHarness<SessionReplacingServer> harness{m_node};
    auto& server{harness.server};
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    server.AddCollateralForTest(MakeCollateral(0));
    server.SetTimedOutForTest();

    server.CheckTimeout();

    // The collateral is consumed only after the reset, outside cs_coinjoin, and a session opened
    // in the meantime is left alone.
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
    class ReadmissionRacingServer : public ChargeRecordingServer
    {
    public:
        using ChargeRecordingServer::ChargeRecordingServer;
        mutable int readmitted_during_consume{-1};
        mutable PoolMessage readmission_message{MSG_NOERR};

        void ConsumeCollateral(const CTransactionRef& txref) const override EXCLUSIVE_LOCKS_REQUIRED(!cs_coinjoin)
        {
            auto* self = const_cast<ReadmissionRacingServer*>(this);
            PoolMessage message{MSG_NOERR};
            readmitted_during_consume = self->TryAdmit(txref, message) ? 1 : 0;
            readmission_message = message;
            ChargeRecordingServer::ConsumeCollateral(txref);
        }
    };

    ServerHarness<ReadmissionRacingServer> harness{m_node};
    auto& server{harness.server};
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
    ServerHarness harness{m_node};
    auto& server{harness.server};
    const auto collateral = MakeCollateral(0);

    for (const PoolState state : {POOL_STATE_ACCEPTING_ENTRIES, POOL_STATE_SIGNING}) {
        BOOST_TEST_CONTEXT("state=" << state)
        {
            server.ResetForTest(state);
            server.AddCollateralForTest(collateral);
            if (state == POOL_STATE_SIGNING) {
                server.SeedEntry(MakeEntry(collateral, /*unsigned_inputs=*/1));
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
}

BOOST_AUTO_TEST_CASE(server_timeout_finalization_waits_for_in_flight_entry)
{
    ServerHarness harness{m_node};
    auto& server{harness.server};

    // Enough entries to finalize without the straggler, but the straggler's entry is still being
    // validated: it must neither be charged as missing nor be left out of the final transaction.
    server.ResetForTest(POOL_STATE_ACCEPTING_ENTRIES);
    for (int i = 0; i <= CoinJoin::GetMinPoolParticipants(); ++i) {
        const auto collateral{MakeCollateral(static_cast<uint32_t>(i))};
        server.AddCollateralForTest(collateral);
        if (i < CoinJoin::GetMinPoolParticipants()) {
            auto entry{MakeEntry(collateral)};
            entry.vecTxDSIn[0].prevout.n = static_cast<uint32_t>(200 + i);
            server.SeedEntry(std::move(entry));
        }
    }
    server.SetTimedOutForTest();

    const int session_id = server.MarkMessageInFlightForTest();
    server.CreateFinalTransaction(session_id, /*charge_fees=*/true);
    BOOST_CHECK_EQUAL(server.GetState(), int{POOL_STATE_ACCEPTING_ENTRIES});
    BOOST_CHECK(server.consumed_collaterals.empty());

    // Once the entry has been handled, the session finalizes without it. (None of the seeded
    // participants is connected, so relaying the final transaction then resets the pool.)
    server.ClearMessageInFlightForTest(session_id);
    server.CreateFinalTransaction(session_id, /*charge_fees=*/true);
    BOOST_CHECK(server.GetState() != int{POOL_STATE_ACCEPTING_ENTRIES});
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
BOOST_AUTO_TEST_CASE(broadcasttx_expiry_height_logic)
{
    // Build a valid-looking CCoinJoinBroadcastTx with confirmed height
    CCoinJoinBroadcastTx dstx;
    {
        CMutableTransaction mtx;
        const int participants = std::max(3, CoinJoin::GetMinPoolParticipants());
        for (int i = 0; i < participants; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::TWO, i));
            mtx.vout.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i)));
        }
        dstx.tx = MakeTransactionRef(mtx);
        dstx.m_protxHash = uint256::ONE;
        // mark as confirmed at height 100
        dstx.SetConfirmedHeight(100);
    }

    // Minimal CBlockIndex with required fields
    // Create a minimal block index to satisfy the interface
    CBlockIndex index;
    uint256 blk_hash = uint256S("03");
    index.nHeight = 125; // 125 - 100 == 25 > 24 → expired by height
    index.phashBlock = &blk_hash;
    BOOST_CHECK(dstx.IsExpired(&index, *Assert(m_node.chainlocks)));
}

// Helper to create a denominated CTxIn with a specific denomination value
static CTxIn MakeDenomInput(uint8_t index)
{
    CTxIn in;
    in.prevout = COutPoint(uint256::ONE, index);
    return in;
}

// Helper to create a denominated CTxOut
static CTxOut MakeDenomOutput(CAmount nAmount, uint8_t tag = 0x01)
{
    return CTxOut{nAmount, P2PKHScript(tag)};
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_valid)
{
    // Valid promotion: 10 inputs of 0.1 DASH → 1 output of 1.0 DASH
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    // Get the 0.1 DASH denomination (index 2: 1 << 2 = 4)
    const int nSmallerDenom = 1 << 2;  // 0.1 DASH
    const int nLargerDenom = 1 << 1;   // 1.0 DASH
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    BOOST_CHECK(CoinJoin::IsValidDenomination(nSmallerDenom));
    BOOST_CHECK(CoinJoin::IsValidDenomination(nLargerDenom));

    // Create 10 inputs of smaller denomination
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxIn.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }

    // Create 1 output of larger denomination
    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x01));

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidatePromotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_wrong_input_count)
{
    // Invalid: only 9 inputs instead of 10
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;  // 0.1 DASH
    const int nLargerDenom = 1 << 1;   // 1.0 DASH
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    // Create only 9 inputs
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO - 1; ++i) {
        vecTxIn.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }

    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x01));

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_wrong_output_count)
{
    // Invalid: 2 outputs instead of 1
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;
    const int nLargerDenom = 1 << 1;
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxIn.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }

    // Two outputs instead of one
    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x01));
    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x02));

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_non_adjacent_denoms)
{
    // Invalid: trying to promote 0.01 to 1.0 (not adjacent)
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 3;  // 0.01 DASH
    const int nLargerDenom = 1 << 1;   // 1.0 DASH (not adjacent to 0.01)
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxIn.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }

    vecTxOut.push_back(MakeDenomOutput(nLargerAmount, 0x01));

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_demotion_entry_valid)
{
    // Valid demotion: 1 input of 1.0 DASH → 10 outputs of 0.1 DASH
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;  // 0.1 DASH
    const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(nSmallerDenom);
    const int nLargerDenom = 1 << 1;   // 1.0 DASH

    BOOST_CHECK(CoinJoin::IsValidDenomination(nSmallerDenom));
    BOOST_CHECK(CoinJoin::IsValidDenomination(nLargerDenom));
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(nSmallerDenom, nLargerDenom));

    // 1 input of larger denomination
    vecTxIn.push_back(MakeDenomInput(0));

    // 10 outputs of smaller denomination
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxOut.push_back(MakeDenomOutput(nSmallerAmount, static_cast<uint8_t>(i)));
    }

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidateDemotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_demotion_entry_wrong_input_count)
{
    // Invalid: 2 inputs instead of 1
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;
    const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(nSmallerDenom);

    // 2 inputs instead of 1
    vecTxIn.push_back(MakeDenomInput(0));
    vecTxIn.push_back(MakeDenomInput(1));

    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxOut.push_back(MakeDenomOutput(nSmallerAmount, static_cast<uint8_t>(i)));
    }

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidateDemotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_demotion_entry_wrong_output_count)
{
    // Invalid: 9 outputs instead of 10
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nSmallerDenom = 1 << 2;
    const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(nSmallerDenom);

    vecTxIn.push_back(MakeDenomInput(0));

    // Only 9 outputs
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO - 1; ++i) {
        vecTxOut.push_back(MakeDenomOutput(nSmallerAmount, static_cast<uint8_t>(i)));
    }

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidateDemotionEntry(vecTxIn, vecTxOut, nSmallerDenom, nMessageID));
}

BOOST_AUTO_TEST_CASE(validate_demotion_entry_largest_denom_has_no_source)
{
    // Invalid: nothing demotes into 10 DASH, there is no larger denomination to split
    std::vector<CTxIn> vecTxIn;
    std::vector<CTxOut> vecTxOut;

    const int nLargestDenom = 1 << 0;  // 10 DASH
    const CAmount nLargestAmount = CoinJoin::DenominationToAmount(nLargestDenom);
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(nLargestDenom), 0);

    vecTxIn.push_back(MakeDenomInput(0));

    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        vecTxOut.push_back(MakeDenomOutput(nLargestAmount, static_cast<uint8_t>(i)));
    }

    PoolMessage nMessageID = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidateDemotionEntry(vecTxIn, vecTxOut, nLargestDenom, nMessageID));
    BOOST_CHECK(nMessageID == ERR_DENOM);
}

BOOST_AUTO_TEST_CASE(denomination_adjacency_checks)
{
    // Test AreAdjacentDenominations function
    // Denomination indices: 0=10.0, 1=1.0, 2=0.1, 3=0.01, 4=0.001

    // Adjacent pairs should return true
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(1 << 0, 1 << 1));  // 10.0 and 1.0
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(1 << 1, 1 << 2));  // 1.0 and 0.1
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(1 << 2, 1 << 3));  // 0.1 and 0.01
    BOOST_CHECK(CoinJoin::AreAdjacentDenominations(1 << 3, 1 << 4));  // 0.01 and 0.001

    // Non-adjacent pairs should return false
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 0, 1 << 2)); // 10.0 and 0.1 (skip 1.0)
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 1, 1 << 3)); // 1.0 and 0.01 (skip 0.1)
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 0, 1 << 4)); // 10.0 and 0.001

    // Same denomination is not adjacent to itself
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 2, 1 << 2));

    // Invalid denominations
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(0, 1 << 1));
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(1 << 1, 0));
    BOOST_CHECK(!CoinJoin::AreAdjacentDenominations(999, 1 << 1));
}

BOOST_AUTO_TEST_CASE(get_adjacent_denomination_helpers)
{
    // Test GetLargerAdjacentDenom and GetSmallerAdjacentDenom
    // Indices: 0=10.0, 1=1.0, 2=0.1, 3=0.01, 4=0.001

    // GetLargerAdjacentDenom
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 1), 1 << 0);  // 1.0 → 10.0
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 2), 1 << 1);  // 0.1 → 1.0
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 3), 1 << 2);  // 0.01 → 0.1
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 4), 1 << 3);  // 0.001 → 0.01
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(1 << 0), 0);       // 10.0 has no larger
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(0), 0);            // Invalid denominations
    BOOST_CHECK_EQUAL(CoinJoin::GetLargerAdjacentDenom(999), 0);          // Invalid denominations

    // GetSmallerAdjacentDenom
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 0), 1 << 1); // 10.0 → 1.0
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 1), 1 << 2); // 1.0 → 0.1
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 2), 1 << 3); // 0.1 → 0.01
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 3), 1 << 4); // 0.01 → 0.001
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(1 << 4), 0);      // 0.001 has no smaller
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(0), 0);           // Invalid denominations
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(999), 0);         // Invalid denominations
}

BOOST_AUTO_TEST_CASE(isvalidstructure_postfork_unbalanced_valid)
{
    // Post-V24: Unbalanced vin/vout (promotion: 10 inputs, 1 output) should be valid
    // We need a mock pindex that signals V24 active - for this test we use nullptr which means pre-fork
    // This test validates that the structure check correctly identifies promotion structure

    CCoinJoinBroadcastTx promo;
    {
        CMutableTransaction mtx;
        // Promotion: 10 inputs of smaller denom -> 1 output of larger denom
        const int nInputCount = CoinJoin::PROMOTION_RATIO;
        const CAmount nLargerAmount = CoinJoin::DenominationToAmount(1 << 1);  // 1.0 DASH

        for (int i = 0; i < nInputCount; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
        }
        // 1 output of larger denom
        CTxOut out{nLargerAmount, P2PKHScript(0x01)};
        mtx.vout.push_back(out);

        promo.tx = MakeTransactionRef(mtx);
        promo.m_protxHash = uint256::ONE;
    }

    // Pre-V24 (nullptr): unbalanced should fail, but be reported as possibly valid post-V24
    // so relaying peers aren't fully punished around the activation boundary
    bool fPossiblyValidPostV24{false};
    BOOST_CHECK(!promo.IsValidStructure(nullptr, *Assert(m_node.chainman), &fPossiblyValidPostV24));
    BOOST_CHECK(fPossiblyValidPostV24);

    // Note: Post-V24 test would require a valid CBlockIndex with V24 deployment active
    // which requires more setup. The above confirms pre-fork rejection works.
}

BOOST_AUTO_TEST_CASE(isvalidstructure_demotion_structure)
{
    // Demotion: 1 input of larger denom -> 10 outputs of smaller denom
    CCoinJoinBroadcastTx demo;
    {
        CMutableTransaction mtx;
        const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(1 << 2); // 0.1 DASH

        // 1 input
        CTxIn in;
        in.prevout = COutPoint(uint256::ONE, 0);
        mtx.vin.push_back(in);

        // 10 outputs of smaller denom
        for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
            CTxOut out{nSmallerAmount, P2PKHScript(static_cast<uint8_t>(i))};
            mtx.vout.push_back(out);
        }

        demo.tx = MakeTransactionRef(mtx);
        demo.m_protxHash = uint256::ONE;
    }

    // Pre-V24 (nullptr): unbalanced should fail. A single-entry demotion DSTX has just 1
    // input - below GetMinPoolParticipants() under either ruleset - so it must not be
    // reported as possibly valid post-V24 either
    bool fPossiblyValidPostV24{true};
    BOOST_CHECK(!demo.IsValidStructure(nullptr, *Assert(m_node.chainman), &fPossiblyValidPostV24));
    BOOST_CHECK(!fPossiblyValidPostV24);

    // An unbalanced tx that is garbage under post-V24 rules too (non-denominated output)
    // must not be reported as possibly valid
    CCoinJoinBroadcastTx garbage;
    {
        CMutableTransaction mtx;
        for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
        }
        CTxOut out{1234567, P2PKHScript(0x01)}; // not a denomination
        mtx.vout.push_back(out);
        garbage.tx = MakeTransactionRef(mtx);
        garbage.m_protxHash = uint256::ONE;
    }
    fPossiblyValidPostV24 = true;
    BOOST_CHECK(!garbage.IsValidStructure(nullptr, *Assert(m_node.chainman), &fPossiblyValidPostV24));
    BOOST_CHECK(!fPossiblyValidPostV24);
}

BOOST_AUTO_TEST_CASE(input_limit_prefork)
{
    // Pre-V24: max inputs = GetMaxPoolParticipants() * COINJOIN_ENTRY_MAX_SIZE
    // Typically 20 * 9 = 180
    const size_t nMaxPreFork = CoinJoin::GetMaxPoolParticipants() * COINJOIN_ENTRY_MAX_SIZE;
    BOOST_CHECK_EQUAL(nMaxPreFork, 180);

    // Transaction with exactly max inputs should be valid
    CCoinJoinBroadcastTx maxValid;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < nMaxPreFork; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
            CTxOut out{CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256))};
            mtx.vout.push_back(out);
        }
        maxValid.tx = MakeTransactionRef(mtx);
        maxValid.m_protxHash = uint256::ONE;
    }
    BOOST_CHECK(maxValid.IsValidStructure(nullptr, *Assert(m_node.chainman)));

    // Transaction with max+1 inputs should be invalid
    CCoinJoinBroadcastTx tooMany;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < nMaxPreFork + 1; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
            CTxOut out{CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256))};
            mtx.vout.push_back(out);
        }
        tooMany.tx = MakeTransactionRef(mtx);
        tooMany.m_protxHash = uint256::ONE;
    }
    BOOST_CHECK(!tooMany.IsValidStructure(nullptr, *Assert(m_node.chainman)));
}

BOOST_AUTO_TEST_CASE(input_limit_postfork_constants)
{
    // Post-V24: max inputs = GetMaxPoolParticipants() * PROMOTION_RATIO
    // Typically 20 * 10 = 200
    const size_t nMaxPostFork = CoinJoin::GetMaxPoolParticipants() * CoinJoin::PROMOTION_RATIO;
    BOOST_CHECK_EQUAL(nMaxPostFork, 200);

    // Verify the increase from pre-fork
    const size_t nMaxPreFork = CoinJoin::GetMaxPoolParticipants() * COINJOIN_ENTRY_MAX_SIZE;
    BOOST_CHECK(nMaxPostFork > nMaxPreFork);
    BOOST_CHECK_EQUAL(nMaxPostFork - nMaxPreFork, 20); // Increase of 20
}

BOOST_AUTO_TEST_CASE(output_limit_postfork)
{
    // Post-V24 the vin/vout counts no longer bound each other, so outputs get their own cap
    // of GetMaxPoolParticipants() * PROMOTION_RATIO. Exercised through the
    // fPossiblyValidPostV24 flag: a tx that busts the output cap must not be reported as
    // possibly valid under post-V24 rules either.
    const size_t nMaxPostFork = CoinJoin::GetMaxPoolParticipants() * CoinJoin::PROMOTION_RATIO;
    const CAmount nSmallestDenom = CoinJoin::GetSmallestDenomination();

    auto MakeUnbalancedTx = [&](size_t nInputs, size_t nOutputs) {
        CCoinJoinBroadcastTx dstx;
        CMutableTransaction mtx;
        for (size_t i = 0; i < nInputs; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
        }
        for (size_t i = 0; i < nOutputs; ++i) {
            mtx.vout.push_back(CTxOut{nSmallestDenom, P2PKHScript(static_cast<uint8_t>(i % 256))});
        }
        dstx.tx = MakeTransactionRef(mtx);
        dstx.m_protxHash = uint256::ONE;
        return dstx;
    };

    // Outputs at the cap, inputs the matching count for an all-demotion session: rejected
    // pre-V24, possibly valid post-V24
    bool fPossiblyValidPostV24{false};
    const auto withinCaps = MakeUnbalancedTx(nMaxPostFork / CoinJoin::PROMOTION_RATIO, nMaxPostFork);
    BOOST_CHECK(!withinCaps.IsValidStructure(nullptr, *Assert(m_node.chainman), &fPossiblyValidPostV24));
    BOOST_CHECK(fPossiblyValidPostV24);

    // The same shape scaled past the cap: rejected under either ruleset, and by the cap alone -
    // it satisfies the input/output count relation (see isvalidstructure_postv24_count_relations)
    fPossiblyValidPostV24 = true;
    const auto tooManyOutputs = MakeUnbalancedTx(nMaxPostFork / CoinJoin::PROMOTION_RATIO + 1, nMaxPostFork + 10);
    BOOST_CHECK(!tooManyOutputs.IsValidStructure(nullptr, *Assert(m_node.chainman), &fPossiblyValidPostV24));
    BOOST_CHECK(!fPossiblyValidPostV24);
}

BOOST_AUTO_TEST_CASE(validate_final_tx_composition)
{
    constexpr size_t R = CoinJoin::PROMOTION_RATIO;

    // Standard-only session: N inputs, N outputs, no larger-denom in/outs
    BOOST_CHECK(CoinJoin::ValidateFinalTxComposition(5, 5, 0, 0));

    // 3 standard + 1 promotion: 3 + 10 inputs, 3 + 1 outputs (1 at larger denom)
    BOOST_CHECK(CoinJoin::ValidateFinalTxComposition(3 + R, 3 + 1, 0, 1));

    // 3 standard + 1 demotion: 3 + 1 inputs (1 at larger denom), 3 + 10 outputs
    BOOST_CHECK(CoinJoin::ValidateFinalTxComposition(3 + 1, 3 + R, 1, 0));

    // 3 standard + 1 promotion + 1 demotion
    BOOST_CHECK(CoinJoin::ValidateFinalTxComposition(3 + R + 1, 3 + 1 + R, 1, 1));

    // Promotion-only shape is composable (the side-coverage invariant - each side of the
    // session denom occupied by nobody or at least two participants - is enforced by the
    // server in IsSessionReady/CheckPool, not by the composition check)
    BOOST_CHECK(CoinJoin::ValidateFinalTxComposition(R, 1, 0, 1));

    // Not enough session-denom inputs to back the promotion output
    BOOST_CHECK(!CoinJoin::ValidateFinalTxComposition(R - 1, 1, 0, 1));

    // Not enough session-denom outputs to back the demotion input
    BOOST_CHECK(!CoinJoin::ValidateFinalTxComposition(1, R - 1, 1, 0));

    // Larger-denom counts exceeding the totals are inconsistent
    BOOST_CHECK(!CoinJoin::ValidateFinalTxComposition(5, 5, 6, 0));
    BOOST_CHECK(!CoinJoin::ValidateFinalTxComposition(5, 5, 0, 6));

    // Two promotions with only one ratio's worth of session inputs
    BOOST_CHECK(!CoinJoin::ValidateFinalTxComposition(R, 2, 0, 2));
}

BOOST_AUTO_TEST_CASE(promotion_demotion_value_preservation)
{
    // Verify that 10 smaller = 1 larger (value is preserved exactly)
    // CoinJoin denominations are designed so that 10 * smaller == larger
    // e.g., 10 * (0.1 DASH + 100 sat) = 1.0 DASH + 1000 sat

    const CAmount nSmallerAmount = CoinJoin::DenominationToAmount(1 << 2); // 0.1 DASH
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(1 << 1);  // 1.0 DASH

    // Value must match EXACTLY for promotion/demotion to preserve value
    BOOST_CHECK_EQUAL(nSmallerAmount * CoinJoin::PROMOTION_RATIO, nLargerAmount);

    // Verify for all adjacent denomination pairs
    for (int i = 0; i < 4; ++i) {
        const int nLargerDenom = 1 << i;
        const int nSmallerDenom = 1 << (i + 1);
        const CAmount nLarger = CoinJoin::DenominationToAmount(nLargerDenom);
        const CAmount nSmaller = CoinJoin::DenominationToAmount(nSmallerDenom);

        BOOST_CHECK(nLarger > nSmaller);
        // The denominations are designed so 10 * smaller == larger exactly
        BOOST_CHECK_EQUAL(nSmaller * CoinJoin::PROMOTION_RATIO, nLarger);
    }
}

BOOST_AUTO_TEST_CASE(isvalidstructure_mixed_session_postfork)
{
    // Post-V24: A transaction with mixed entry types should be valid
    // (some participants doing 1:1, others doing promotion/demotion)
    // This tests that the structure validation allows mixed input/output counts

    CCoinJoinBroadcastTx mixed;
    {
        CMutableTransaction mtx;
        // Simulate a mixed session:
        // - 3 standard participants: 3 inputs, 3 outputs (smallest denom)
        // - 1 promotion participant: 10 inputs, 1 output
        // Total: 13 inputs, 4 outputs

        const CAmount nSmallestDenom = CoinJoin::GetSmallestDenomination();
        const CAmount nSecondSmallest = CoinJoin::DenominationToAmount(1 << 3); // 0.01 DASH

        // Standard participants (3 x 1:1)
        for (int i = 0; i < 3; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
            mtx.vout.push_back(CTxOut{nSmallestDenom, P2PKHScript(static_cast<uint8_t>(i))});
        }

        // Promotion participant (10 inputs -> 1 output)
        for (int i = 3; i < 13; ++i) {
            CTxIn in;
            in.prevout = COutPoint(uint256::ONE, static_cast<uint32_t>(i));
            mtx.vin.push_back(in);
        }
        // One output for the promotion (larger denom)
        mtx.vout.push_back(CTxOut{nSecondSmallest, P2PKHScript(0x0D)});

        mixed.tx = MakeTransactionRef(mtx);
        mixed.m_protxHash = uint256::ONE;
    }

    // Pre-V24: Should fail (unbalanced)
    BOOST_CHECK(!mixed.IsValidStructure(nullptr, *Assert(m_node.chainman)));

    // Note: Post-V24 testing requires a valid CBlockIndex with V24 deployment active
    // which requires more complex test setup with chainstate. The above verifies
    // that pre-fork rejection works correctly.
}

BOOST_AUTO_TEST_CASE(entry_type_detection_logic)
{
    // Test the entry type detection logic used in IsValidInOuts
    // Entry types:
    // - STANDARD: vin.size() == vout.size()
    // - PROMOTION: vin.size() == PROMOTION_RATIO && vout.size() == 1
    // - DEMOTION: vin.size() == 1 && vout.size() == PROMOTION_RATIO
    // - INVALID: anything else

    auto detectEntryType = [](size_t vinSize, size_t voutSize) -> std::string {
        if (vinSize == voutSize) {
            return "STANDARD";
        } else if (vinSize == static_cast<size_t>(CoinJoin::PROMOTION_RATIO) && voutSize == 1) {
            return "PROMOTION";
        } else if (vinSize == 1 && voutSize == static_cast<size_t>(CoinJoin::PROMOTION_RATIO)) {
            return "DEMOTION";
        }
        return "INVALID";
    };

    // Standard entries
    BOOST_CHECK_EQUAL(detectEntryType(1, 1), "STANDARD");
    BOOST_CHECK_EQUAL(detectEntryType(5, 5), "STANDARD");
    BOOST_CHECK_EQUAL(detectEntryType(9, 9), "STANDARD");  // Max standard entry

    // Promotion entries
    BOOST_CHECK_EQUAL(detectEntryType(10, 1), "PROMOTION");

    // Demotion entries
    BOOST_CHECK_EQUAL(detectEntryType(1, 10), "DEMOTION");

    // Invalid entries (not valid pre or post fork)
    BOOST_CHECK_EQUAL(detectEntryType(9, 1), "INVALID");   // Wrong ratio
    BOOST_CHECK_EQUAL(detectEntryType(1, 9), "INVALID");   // Wrong ratio
    BOOST_CHECK_EQUAL(detectEntryType(5, 3), "INVALID");   // Random mismatch
    BOOST_CHECK_EQUAL(detectEntryType(2, 10), "INVALID");  // Wrong input count for demotion
    BOOST_CHECK_EQUAL(detectEntryType(10, 2), "INVALID");  // Wrong output count for promotion
    BOOST_CHECK_EQUAL(detectEntryType(20, 2), "INVALID");  // 20:2 is not valid
    BOOST_CHECK_EQUAL(detectEntryType(11, 1), "INVALID");  // 11:1 is not valid promotion
}

BOOST_AUTO_TEST_CASE(validate_entry_session_denom_consistency)
{
    // Test that promotion/demotion validation enforces session denomination consistency
    // For promotion: inputs must be session denom (smaller), output must be larger adjacent
    // For demotion: input must be larger adjacent, outputs must be session denom (smaller)

    const int nSessionDenom = 1 << 2;  // 0.1 DASH (session denom)
    const int nLargerDenom = CoinJoin::GetLargerAdjacentDenom(nSessionDenom);

    BOOST_CHECK(nLargerDenom != 0);
    BOOST_CHECK_EQUAL(nLargerDenom, 1 << 1);  // 1.0 DASH

    // Create a valid promotion entry structure
    std::vector<CTxIn> promoVin;
    std::vector<CTxOut> promoVout;
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        promoVin.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }
    promoVout.push_back(MakeDenomOutput(CoinJoin::DenominationToAmount(nLargerDenom)));

    PoolMessage msg = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidatePromotionEntry(promoVin, promoVout, nSessionDenom, msg));

    // Test with wrong session denom (largest denom can't promote)
    const int nLargestDenom = 1 << 0;  // 10 DASH
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(promoVin, promoVout, nLargestDenom, msg));
    BOOST_CHECK(msg == ERR_DENOM);  // No larger adjacent for 10 DASH

    // Create a valid demotion entry structure
    std::vector<CTxIn> demoVin;
    std::vector<CTxOut> demoVout;
    demoVin.push_back(MakeDenomInput(0));
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        demoVout.push_back(MakeDenomOutput(CoinJoin::DenominationToAmount(nSessionDenom), static_cast<uint8_t>(i)));
    }

    msg = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidateDemotionEntry(demoVin, demoVout, nSessionDenom, msg));
}

BOOST_AUTO_TEST_CASE(isvalidstructure_boundary_input_counts)
{
    // Test boundary conditions for input counts at the pre/post V24 limits
    // Pre-V24: max 180 inputs (20 * 9)
    // Post-V24: max 200 inputs (20 * 10)

    const size_t nPreForkMax = CoinJoin::GetMaxPoolParticipants() * COINJOIN_ENTRY_MAX_SIZE;
    const size_t nPostForkMax = CoinJoin::GetMaxPoolParticipants() * CoinJoin::PROMOTION_RATIO;

    BOOST_CHECK_EQUAL(nPreForkMax, 180);
    BOOST_CHECK_EQUAL(nPostForkMax, 200);

    // Create transaction with exactly 180 inputs (valid pre and post fork)
    CCoinJoinBroadcastTx tx180;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < 180; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::ONE, static_cast<uint32_t>(i)));
            mtx.vout.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256)));
        }
        tx180.tx = MakeTransactionRef(mtx);
        tx180.m_protxHash = uint256::ONE;
    }
    BOOST_CHECK(tx180.IsValidStructure(nullptr, *Assert(m_node.chainman)));  // Pre-fork: valid at boundary

    // Create transaction with 181 inputs (invalid pre-fork, would be valid post-fork)
    CCoinJoinBroadcastTx tx181;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < 181; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::ONE, static_cast<uint32_t>(i)));
            mtx.vout.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256)));
        }
        tx181.tx = MakeTransactionRef(mtx);
        tx181.m_protxHash = uint256::ONE;
    }
    BOOST_CHECK(!tx181.IsValidStructure(nullptr, *Assert(m_node.chainman)));  // Pre-fork: over limit

    // Transaction at post-fork boundary (200 inputs)
    CCoinJoinBroadcastTx tx200;
    {
        CMutableTransaction mtx;
        for (size_t i = 0; i < 200; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::ONE, static_cast<uint32_t>(i)));
            mtx.vout.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256)));
        }
        tx200.tx = MakeTransactionRef(mtx);
        tx200.m_protxHash = uint256::ONE;
    }
    // Pre-fork: over limit (200 > 180)
    BOOST_CHECK(!tx200.IsValidStructure(nullptr, *Assert(m_node.chainman)));

    // Note: Testing post-fork behavior requires a CBlockIndex with V24 active via EHF
}

BOOST_AUTO_TEST_CASE(isvalidstructure_postv24_count_relations)
{
    // The post-V24 branch needs a V24-active CBlockIndex, which this fixture has no chain for.
    // Its verdict is observable all the same: on a pre-V24 tip IsValidStructure reports
    // fPossiblyValidPostV24 exactly when a transaction fails the pre-V24 rules but passes the
    // post-V24 ones, so for any shape that is invalid pre-V24 the flag *is* the post-V24 verdict.
    // Every shape below is invalid pre-V24 - unbalanced, or balanced above the pre-V24 cap.
    const size_t nMaxInOuts = static_cast<size_t>(CoinJoin::GetMaxPoolParticipants()) * CoinJoin::PROMOTION_RATIO;
    const size_t nPreV24Cap = static_cast<size_t>(CoinJoin::GetMaxPoolParticipants()) * COINJOIN_ENTRY_MAX_SIZE;

    auto valid_post_v24 = [this](size_t nIn, size_t nOut) {
        CMutableTransaction mtx;
        for (size_t i = 0; i < nIn; ++i) {
            mtx.vin.emplace_back(COutPoint(uint256::ONE, static_cast<uint32_t>(i)));
        }
        for (size_t i = 0; i < nOut; ++i) {
            mtx.vout.emplace_back(CoinJoin::GetSmallestDenomination(), P2PKHScript(static_cast<uint8_t>(i % 256)));
        }
        CCoinJoinBroadcastTx dstx;
        dstx.tx = MakeTransactionRef(mtx);
        dstx.m_protxHash = uint256::ONE;

        bool fPossiblyValidPostV24{false};
        BOOST_REQUIRE(!dstx.IsValidStructure(nullptr, *Assert(m_node.chainman), &fPossiblyValidPostV24));
        return fPossiblyValidPostV24;
    };

    // Shapes a session can actually produce: p promotions and d demotions over standard entries
    BOOST_CHECK(valid_post_v24(100, 10));                        // 10 promotions
    BOOST_CHECK(valid_post_v24(10, 100));                        // 10 demotions
    BOOST_CHECK(valid_post_v24(19, 10));                         // 1 promotion + 9 standard 1:1
    BOOST_CHECK(valid_post_v24(nMaxInOuts, nMaxInOuts / 10));    // promotions at the cap
    BOOST_CHECK(valid_post_v24(nMaxInOuts / 10, nMaxInOuts));    // demotions at the cap

    // Neither side may exceed PROMOTION_RATIO times the other, even when the difference between
    // them is a legal multiple of PROMOTION_RATIO - 1 (36 and 36 here)
    BOOST_CHECK(!valid_post_v24(39, 3));
    BOOST_CHECK(!valid_post_v24(3, 39));

    // Every promotion adds PROMOTION_RATIO - 1 more inputs than outputs and every demotion the
    // mirror, so a difference that is not a multiple of that step is not composable from entries
    BOOST_CHECK(!valid_post_v24(4, 3));
    BOOST_CHECK(!valid_post_v24(199, 20));

    // Either side over its own cap, with ratio and step both satisfied
    BOOST_CHECK(!valid_post_v24(nMaxInOuts + 10, nMaxInOuts / 10 + 1));
    BOOST_CHECK(!valid_post_v24(nMaxInOuts / 10 + 1, nMaxInOuts + 10));

    // No outputs at all: the denominated-output check above passes vacuously, so the count
    // relation is what has to reject it
    BOOST_CHECK(!valid_post_v24(3, 0));

    // A balanced transaction above the pre-V24 cap is composable post-V24 and must stay valid:
    // every entry of a rebalance session may carry PROMOTION_RATIO coins, so one promotion, one
    // demotion and GetMaxPoolParticipants() - 2 standard entries at that size come to
    // nMaxInOuts - (PROMOTION_RATIO - 1) on both sides. It exceeds the count peers below
    // COINJOIN_REBALANCE_VERSION accept, which is why CanAnnounceDstxTo withholds it from them
    // rather than IsValidStructure rejecting it.
    const size_t nMaxBalanced = nMaxInOuts - (CoinJoin::PROMOTION_RATIO - 1);
    BOOST_REQUIRE(nMaxBalanced > nPreV24Cap);
    BOOST_CHECK(valid_post_v24(nMaxBalanced, nMaxBalanced));
}

// ============================================================================
// Tests for the ShouldPromoteDenoms/ShouldDemoteDenoms decision logic that
// CCoinJoinClientManager::ShouldPromote/ShouldDemote dispatch to
// ============================================================================

namespace {

// Adapters over the live decision functions; both adapters treat every coin as
// fully mixed so the count-based cases below isolate the deficit logic
// (the fully-mixed gates have their own tests)
bool TestShouldPromote(int smallerCount, int largerCount, int goal)
{
    return CoinJoin::ShouldPromoteDenoms(smallerCount, largerCount, /*nSmallerFullyMixedCount=*/smallerCount, goal);
}

bool TestShouldDemote(int largerCount, int smallerCount, int goal)
{
    return CoinJoin::ShouldDemoteDenoms(largerCount, smallerCount, /*nLargerFullyMixedCount=*/largerCount, goal);
}

} // anonymous namespace

BOOST_AUTO_TEST_CASE(should_promote_requires_fully_mixed_coins)
{
    // Deficit logic says promote (100 smaller vs 0 larger), but fewer than
    // PROMOTION_RATIO fully-mixed coins must block the promotion
    BOOST_CHECK(CoinJoin::ShouldPromoteDenoms(100, 0, CoinJoin::PROMOTION_RATIO, 100));
    BOOST_CHECK(!CoinJoin::ShouldPromoteDenoms(100, 0, CoinJoin::PROMOTION_RATIO - 1, 100));
    BOOST_CHECK(!CoinJoin::ShouldPromoteDenoms(100, 0, 0, 100));
}

BOOST_AUTO_TEST_CASE(should_demote_requires_fully_mixed_coin)
{
    // Deficit logic says demote (100 larger vs 0 smaller), but without a fully-mixed
    // coin of the larger denomination the demotion must be blocked
    BOOST_CHECK(CoinJoin::ShouldDemoteDenoms(100, 0, /*nLargerFullyMixedCount=*/1, 100));
    BOOST_CHECK(!CoinJoin::ShouldDemoteDenoms(100, 0, /*nLargerFullyMixedCount=*/0, 100));
}

BOOST_AUTO_TEST_CASE(should_promote_larger_deficit_greater)
{
    const int goal = 100;

    // 60 of smaller, 0 of larger -> should promote (0.1 has surplus, 1.0 needs help)
    // smallerDeficit = 40, largerDeficit = 100, gap = 60 > gap threshold (20 at this goal)
    BOOST_CHECK(TestShouldPromote(60, 0, goal));

    // 100 of smaller, 0 of larger -> should promote (0.1 full, 1.0 empty)
    // smallerDeficit = 0, largerDeficit = 100, gap = 100 > gap threshold (20 at this goal)
    BOOST_CHECK(TestShouldPromote(100, 0, goal));
}

BOOST_AUTO_TEST_CASE(should_promote_below_half_goal_false)
{
    const int goal = 100;

    // 30 of smaller, 0 of larger -> should NOT promote (30 < 50 = halfGoal)
    BOOST_CHECK(!TestShouldPromote(30, 0, goal));

    // 49 of smaller, 0 of larger -> should NOT promote (49 < 50 = halfGoal)
    BOOST_CHECK(!TestShouldPromote(49, 0, goal));

    // 50 of smaller, 0 of larger -> CAN promote (50 >= 50 = halfGoal)
    // smallerDeficit = 50, largerDeficit = 100, gap = 50 > gap threshold (20 at this goal)
    BOOST_CHECK(TestShouldPromote(50, 0, goal));
}

BOOST_AUTO_TEST_CASE(gap_threshold_reachable_at_every_goal)
{
    // The gap between two denominations can never exceed the goal, so a threshold that equals
    // the goal makes promotion/demotion unsatisfiable. Check the extreme imbalance converts at
    // the smallest configurable goal, which a constant threshold of 10 silently disabled.
    const int min_goal = MIN_COINJOIN_DENOMS_GOAL;
    BOOST_CHECK(TestShouldPromote(min_goal, 0, min_goal));
    BOOST_CHECK(TestShouldDemote(min_goal, 0, min_goal));

    // The gap still has to be cleared: at the minimum goal a one-coin difference is not enough
    BOOST_CHECK(!TestShouldPromote(min_goal, min_goal - 1, min_goal));
    BOOST_CHECK(!TestShouldDemote(min_goal, min_goal - 1, min_goal));

    // The default goal keeps the threshold it was tuned with
    BOOST_CHECK_EQUAL(CoinJoin::GetGapThreshold(DEFAULT_COINJOIN_DENOMS_GOAL), 10);
}

BOOST_AUTO_TEST_CASE(should_promote_small_gap_false)
{
    const int goal = 100;

    // 90 smaller, 80 larger -> deficits are 10 and 20, gap = 10
    // 20 > 10 + 20 is false, so no promotion
    BOOST_CHECK(!TestShouldPromote(90, 80, goal));

    // 85 smaller, 80 larger -> deficits are 15 and 20, gap = 5
    // 20 > 15 + 20 is false, so no promotion
    BOOST_CHECK(!TestShouldPromote(85, 80, goal));
}

BOOST_AUTO_TEST_CASE(should_demote_smaller_deficit_greater)
{
    const int goal = 100;

    // 60 of larger, 0 of smaller -> should demote (1.0 has surplus, 0.1 needs help)
    // largerDeficit = 40, smallerDeficit = 100, gap = 60 > gap threshold (20 at this goal)
    BOOST_CHECK(TestShouldDemote(60, 0, goal));

    // 99 of larger, 0 of smaller -> should demote
    // largerDeficit = 1, smallerDeficit = 100, gap = 99 > gap threshold (20 at this goal)
    BOOST_CHECK(TestShouldDemote(99, 0, goal));
}

BOOST_AUTO_TEST_CASE(should_demote_below_half_goal_false)
{
    const int goal = 100;

    // 30 of larger, 0 of smaller -> should NOT demote (30 < 50 = halfGoal)
    BOOST_CHECK(!TestShouldDemote(30, 0, goal));

    // 49 of larger, 0 of smaller -> should NOT demote (49 < 50 = halfGoal)
    BOOST_CHECK(!TestShouldDemote(49, 0, goal));

    // 50 of larger, 0 of smaller -> CAN demote (50 >= 50 = halfGoal)
    // largerDeficit = 50, smallerDeficit = 100, gap = 50 > gap threshold (20 at this goal)
    BOOST_CHECK(TestShouldDemote(50, 0, goal));
}

BOOST_AUTO_TEST_CASE(promote_demote_mutually_exclusive)
{
    // Test various distributions - at most one should be true
    auto testMutualExclusivity = [](int smallerCount, int largerCount) {
        const int testGoal = 100;
        bool promote = TestShouldPromote(smallerCount, largerCount, testGoal);
        bool demote = TestShouldDemote(largerCount, smallerCount, testGoal);
        // At most one can be true (XOR or neither)
        BOOST_CHECK(!(promote && demote));
        return std::make_pair(promote, demote);
    };

    // Equal counts - neither
    auto [p1, d1] = testMutualExclusivity(100, 100);
    BOOST_CHECK(!p1 && !d1);

    // 90/90 - neither (close to goal, small gap)
    auto [p2, d2] = testMutualExclusivity(90, 90);
    BOOST_CHECK(!p2 && !d2);

    // Large imbalance toward smaller - promote only
    auto [p3, d3] = testMutualExclusivity(100, 0);
    BOOST_CHECK(p3 && !d3);

    // Large imbalance toward larger - demote only
    auto [p4, d4] = testMutualExclusivity(0, 100);
    BOOST_CHECK(!p4 && d4);

    // Mid-range cases from the plan
    auto [p5, d5] = testMutualExclusivity(0, 60);  // 0.1=0, 1.0=60 -> should demote
    BOOST_CHECK(!p5 && d5);

    auto [p6, d6] = testMutualExclusivity(60, 0);  // 0.1=60, 1.0=0 -> should promote
    BOOST_CHECK(p6 && !d6);
}

BOOST_AUTO_TEST_CASE(decision_logic_example_cases_from_plan)
{
    // Test the specific examples from the implementation plan
    const int goal = 100;

    // | 1.0 count | 0.1 count | Action |
    // | 99        | 0         | Demote |
    BOOST_CHECK(TestShouldDemote(99, 0, goal));
    BOOST_CHECK(!TestShouldPromote(0, 99, goal));  // 0.1 has 0, can't promote

    // | 60        | 0         | Demote |
    BOOST_CHECK(TestShouldDemote(60, 0, goal));

    // | 30        | 0         | Nothing (1.0 < halfGoal) |
    BOOST_CHECK(!TestShouldDemote(30, 0, goal));
    BOOST_CHECK(!TestShouldPromote(0, 30, goal));

    // | 0         | 60        | Promote |
    BOOST_CHECK(TestShouldPromote(60, 0, goal));

    // | 0         | 30        | Nothing (0.1 < halfGoal) |
    BOOST_CHECK(!TestShouldPromote(30, 0, goal));

    // | 100       | 100       | Nothing (both at goal) |
    BOOST_CHECK(!TestShouldPromote(100, 100, goal));
    BOOST_CHECK(!TestShouldDemote(100, 100, goal));

    // | 90        | 90        | Nothing (deficits equal) |
    BOOST_CHECK(!TestShouldPromote(90, 90, goal));
    BOOST_CHECK(!TestShouldDemote(90, 90, goal));
}

BOOST_AUTO_TEST_CASE(validate_promotion_entry_edge_cases)
{
    // Additional edge cases for ValidatePromotionEntry

    const int nSessionDenom = 1 << 2;  // 0.1 DASH
    const int nLargerDenom = CoinJoin::GetLargerAdjacentDenom(nSessionDenom);
    const CAmount nLargerAmount = CoinJoin::DenominationToAmount(nLargerDenom);

    // Valid case: exactly 10 inputs, 1 output of correct larger denom
    std::vector<CTxIn> validVin;
    std::vector<CTxOut> validVout;
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        validVin.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }
    validVout.push_back(MakeDenomOutput(nLargerAmount));

    PoolMessage msg = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidatePromotionEntry(validVin, validVout, nSessionDenom, msg));

    // Invalid: 9 inputs (wrong count)
    std::vector<CTxIn> wrongCountVin;
    for (int i = 0; i < 9; ++i) {
        wrongCountVin.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(wrongCountVin, validVout, nSessionDenom, msg));

    // Invalid: 11 inputs (wrong count)
    std::vector<CTxIn> tooManyVin;
    for (int i = 0; i < 11; ++i) {
        tooManyVin.push_back(MakeDenomInput(static_cast<uint8_t>(i)));
    }
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(tooManyVin, validVout, nSessionDenom, msg));

    // Invalid: 2 outputs (should be 1)
    std::vector<CTxOut> twoOutputs;
    twoOutputs.push_back(MakeDenomOutput(nLargerAmount / 2));
    twoOutputs.push_back(MakeDenomOutput(nLargerAmount / 2));
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(validVin, twoOutputs, nSessionDenom, msg));

    // Invalid: output is wrong denomination
    std::vector<CTxOut> wrongDenomOut;
    wrongDenomOut.push_back(MakeDenomOutput(CoinJoin::DenominationToAmount(nSessionDenom)));  // Same denom, not larger
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidatePromotionEntry(validVin, wrongDenomOut, nSessionDenom, msg));
}

BOOST_AUTO_TEST_CASE(validate_demotion_entry_edge_cases)
{
    // Additional edge cases for ValidateDemotionEntry

    const int nSessionDenom = 1 << 2;  // 0.1 DASH (output denom)
    const CAmount nSessionAmount = CoinJoin::DenominationToAmount(nSessionDenom);

    // Verify the larger adjacent denom exists for this test to be meaningful
    BOOST_CHECK(CoinJoin::GetLargerAdjacentDenom(nSessionDenom) != 0);

    // Valid case: 1 input of larger denom, 10 outputs of session denom
    std::vector<CTxIn> validVin;
    validVin.push_back(MakeDenomInput(0));

    std::vector<CTxOut> validVout;
    for (int i = 0; i < CoinJoin::PROMOTION_RATIO; ++i) {
        validVout.push_back(MakeDenomOutput(nSessionAmount, static_cast<uint8_t>(i)));
    }

    PoolMessage msg = MSG_NOERR;
    BOOST_CHECK(CoinJoin::ValidateDemotionEntry(validVin, validVout, nSessionDenom, msg));

    // Invalid: 2 inputs (should be 1)
    std::vector<CTxIn> twoInputs;
    twoInputs.push_back(MakeDenomInput(0));
    twoInputs.push_back(MakeDenomInput(1));
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidateDemotionEntry(twoInputs, validVout, nSessionDenom, msg));

    // Invalid: 9 outputs (wrong count)
    std::vector<CTxOut> nineOutputs;
    for (int i = 0; i < 9; ++i) {
        nineOutputs.push_back(MakeDenomOutput(nSessionAmount, static_cast<uint8_t>(i)));
    }
    msg = MSG_NOERR;
    BOOST_CHECK(!CoinJoin::ValidateDemotionEntry(validVin, nineOutputs, nSessionDenom, msg));

    // Test that demotion from smallest denomination (0.001 DASH) fails
    // because there's no smaller denomination to demote to
    const int nSmallestDenom = 1 << 4;  // 0.001 DASH
    BOOST_CHECK_EQUAL(CoinJoin::GetSmallerAdjacentDenom(nSmallestDenom), 0);  // No smaller exists
}

// Build an entry of a given shape; only the input/output counts matter for classification
static CCoinJoinEntry MakeEntry(size_t nInputs, size_t nOutputs)
{
    CCoinJoinEntry entry;
    entry.vecTxDSIn.resize(nInputs);
    entry.vecTxOut.resize(nOutputs);
    return entry;
}

BOOST_AUTO_TEST_CASE(entry_mix_shape_classification)
{
    using CoinJoin::MixShape;
    constexpr size_t R = CoinJoin::PROMOTION_RATIO;

    // Standard entries mix at the session denomination on both sides
    BOOST_CHECK(MakeEntry(3, 3).GetMixShape() == MixShape::STANDARD);
    BOOST_CHECK(MakeEntry(1, 1).GetMixShape() == MixShape::STANDARD);
    BOOST_CHECK(MakeEntry(9, 9).GetMixShape() == MixShape::STANDARD);
    BOOST_CHECK(MakeEntry(3, 3).IsStandardMixingEntry());

    BOOST_CHECK(MakeEntry(R, 1).GetMixShape() == MixShape::PROMOTION);
    BOOST_CHECK(MakeEntry(1, R).GetMixShape() == MixShape::DEMOTION);
    BOOST_CHECK(!MakeEntry(R, 1).IsStandardMixingEntry());
    BOOST_CHECK(!MakeEntry(1, R).IsStandardMixingEntry());

    // An empty entry occupies neither side - it must not pass as a standard mixer, or it
    // could take a participant slot while providing no cover at all
    BOOST_CHECK(MakeEntry(0, 0).GetMixShape() == MixShape::UNKNOWN);
    BOOST_CHECK(!MakeEntry(0, 0).IsStandardMixingEntry());
    BOOST_CHECK(MakeEntry(R, 0).GetMixShape() == MixShape::UNKNOWN);
    BOOST_CHECK(MakeEntry(0, R).GetMixShape() == MixShape::UNKNOWN);

    // Ratios that are neither balanced nor a valid promotion/demotion
    BOOST_CHECK(MakeEntry(9, 1).GetMixShape() == MixShape::UNKNOWN);
    BOOST_CHECK(MakeEntry(1, 9).GetMixShape() == MixShape::UNKNOWN);
    BOOST_CHECK(MakeEntry(R + 1, 1).GetMixShape() == MixShape::UNKNOWN);
    BOOST_CHECK(MakeEntry(R, 2).GetMixShape() == MixShape::UNKNOWN);
}

BOOST_AUTO_TEST_CASE(mix_side_counts_invariant)
{
    using CoinJoin::MixShape;
    using CoinJoin::MixSideCounts;

    // Each side of the session denomination must be occupied by nobody or by at least two
    // participants; exactly one is the only forbidden count. Standard entries occupy both
    // sides, promotions only the input side, demotions only the output side.
    const auto sides = [](std::initializer_list<MixShape> shapes) {
        MixSideCounts counts;
        for (const auto shape : shapes) counts.Add(shape);
        return counts;
    };

    // Allowed compositions
    BOOST_CHECK(sides({MixShape::STANDARD, MixShape::STANDARD, MixShape::STANDARD}).IsCovered());
    BOOST_CHECK(sides({MixShape::STANDARD, MixShape::STANDARD, MixShape::PROMOTION}).IsCovered());
    BOOST_CHECK(sides({MixShape::STANDARD, MixShape::STANDARD, MixShape::DEMOTION}).IsCovered());
    // Rebalancers can cover each other without any 1:1 mixer present
    BOOST_CHECK(sides({MixShape::PROMOTION, MixShape::PROMOTION}).IsCovered());
    BOOST_CHECK(sides({MixShape::DEMOTION, MixShape::DEMOTION}).IsCovered());
    BOOST_CHECK(sides({MixShape::PROMOTION, MixShape::PROMOTION, MixShape::DEMOTION, MixShape::DEMOTION}).IsCovered());
    // An empty session is trivially covered
    BOOST_CHECK(sides({}).IsCovered());

    // Forbidden: the lone promoter is the only participant with session-denom inputs, so its
    // ten inputs would be identifiable as a group on-chain
    BOOST_CHECK(!sides({MixShape::PROMOTION}).IsCovered());
    BOOST_CHECK(!sides({MixShape::PROMOTION, MixShape::DEMOTION, MixShape::DEMOTION}).IsCovered());
    // Mirror image: the lone demoter is the only one receiving session-denom outputs
    BOOST_CHECK(!sides({MixShape::DEMOTION}).IsCovered());
    BOOST_CHECK(!sides({MixShape::DEMOTION, MixShape::PROMOTION, MixShape::PROMOTION}).IsCovered());
    // A single standard mixer alone is uncovered on both sides
    BOOST_CHECK(!sides({MixShape::STANDARD}).IsCovered());
    // One standard mixer plus one promoter leaves the output side with a lone occupant
    BOOST_CHECK(!sides({MixShape::STANDARD, MixShape::PROMOTION}).IsCovered());

    // Entries that occupy no side never count toward coverage
    const auto unknown_only = sides({MixShape::UNKNOWN, MixShape::UNKNOWN});
    BOOST_CHECK_EQUAL(unknown_only.inputs, 0);
    BOOST_CHECK_EQUAL(unknown_only.outputs, 0);
}

BOOST_AUTO_TEST_CASE(dsa_rebalance_flag_version_gated_serialization)
{
    // The dsa flags field is only serialized between peers at or above
    // COINJOIN_REBALANCE_VERSION; the wire format for older peers must be unchanged
    CMutableTransaction txCollateral;
    txCollateral.vin.emplace_back(COutPoint(uint256::ONE, 0));
    txCollateral.vout.emplace_back(CoinJoin::GetCollateralAmount(), P2PKHScript(0x01));

    const CCoinJoinAccept dsaPlain(1 << 2, txCollateral);
    const CCoinJoinAccept dsaPromotion(1 << 2, txCollateral, CCoinJoinAccept::FLAG_PROMOTION);
    const CCoinJoinAccept dsaDemotion(1 << 2, txCollateral, CCoinJoinAccept::FLAG_DEMOTION);

    BOOST_CHECK(!dsaPlain.IsRebalance());
    BOOST_CHECK(dsaPromotion.IsPromotion() && !dsaPromotion.IsDemotion() && dsaPromotion.IsRebalance());
    BOOST_CHECK(dsaDemotion.IsDemotion() && !dsaDemotion.IsPromotion() && dsaDemotion.IsRebalance());

    // A participant mixes in exactly one direction
    BOOST_CHECK(dsaPlain.HasValidFlags());
    BOOST_CHECK(dsaPromotion.HasValidFlags());
    BOOST_CHECK(dsaDemotion.HasValidFlags());
    const CCoinJoinAccept dsaBoth(1 << 2, txCollateral,
                                  CCoinJoinAccept::FLAG_PROMOTION | CCoinJoinAccept::FLAG_DEMOTION);
    BOOST_CHECK(!dsaBoth.HasValidFlags());

    // Old-version stream: the flags are silently dropped and the encoding matches a
    // flag-less dsa byte for byte
    {
        CDataStream ssPlain(SER_NETWORK, COINJOIN_REBALANCE_VERSION - 1);
        ssPlain << dsaPlain;
        CDataStream ssRebalance(SER_NETWORK, COINJOIN_REBALANCE_VERSION - 1);
        ssRebalance << dsaPromotion;
        BOOST_CHECK(std::equal(ssPlain.begin(), ssPlain.end(), ssRebalance.begin(), ssRebalance.end()));

        CCoinJoinAccept dsaDecoded;
        ssRebalance >> dsaDecoded;
        BOOST_CHECK_EQUAL(dsaDecoded.nDenom, dsaPromotion.nDenom);
        BOOST_CHECK(!dsaDecoded.IsRebalance());
    }

    // New-version stream: each direction round-trips distinctly
    for (const auto& dsa : {dsaPromotion, dsaDemotion}) {
        CDataStream ss(SER_NETWORK, COINJOIN_REBALANCE_VERSION);
        ss << dsa;
        CCoinJoinAccept dsaDecoded;
        ss >> dsaDecoded;
        BOOST_CHECK_EQUAL(dsaDecoded.nDenom, dsa.nDenom);
        BOOST_CHECK_EQUAL(dsaDecoded.IsPromotion(), dsa.IsPromotion());
        BOOST_CHECK_EQUAL(dsaDecoded.IsDemotion(), dsa.IsDemotion());
    }

    // New-version encoding is exactly one byte longer than the legacy encoding
    {
        CDataStream ssOld(SER_NETWORK, COINJOIN_REBALANCE_VERSION - 1);
        ssOld << dsaPlain;
        CDataStream ssNew(SER_NETWORK, COINJOIN_REBALANCE_VERSION);
        ssNew << dsaPlain;
        BOOST_CHECK_EQUAL(ssNew.size(), ssOld.size() + 1);
    }
}

BOOST_AUTO_TEST_CASE(honest_session_always_covers_both_sides)
{
    using CoinJoin::MixShape;
    using CoinJoin::MixSideCounts;

    // Whatever mix of rebalancers joins, a session holding the minimum number of 1:1 mixers
    // covers both sides, so the invariant never rejects an ordinary session
    const int nMin = CoinJoin::GetMinPoolParticipants();
    BOOST_CHECK(nMin >= 2);

    for (int nPromoters = 0; nPromoters <= CoinJoin::GetMaxPoolParticipants() - nMin; ++nPromoters) {
        MixSideCounts counts;
        for (int i = 0; i < nMin; ++i) counts.Add(MixShape::STANDARD);
        for (int i = 0; i < nPromoters; ++i) counts.Add(MixShape::PROMOTION);
        BOOST_CHECK(counts.IsCovered());
    }
}
BOOST_AUTO_TEST_SUITE_END()
