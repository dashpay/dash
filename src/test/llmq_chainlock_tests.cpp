// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>

#include <evo/cbtx.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <masternode/meta.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netaddress.h>
#include <spork.h>
#include <streams.h>
#include <util/strencodings.h>
#include <validation.h>
#include <version.h>

#include <chainlock/chainlock.h>
#include <chainlock/handler.h>
#include <llmq/context.h>
#include <msg_result.h>
#include <protocol.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <vector>

using chainlock::ChainLockSig;
using namespace llmq;
using namespace llmq::testutils;

namespace {
constexpr size_t MAX_SEEN_CHAINLOCKS{2500};
constexpr size_t RECENT_CHAINLOCKS_TO_RETAIN{2};
} // namespace

BOOST_AUTO_TEST_SUITE(llmq_chainlock_tests)

BOOST_AUTO_TEST_CASE(chainlock_construction_test)
{
    // Test default constructor
    ChainLockSig clsig1;
    BOOST_CHECK(clsig1.IsNull());
    BOOST_CHECK_EQUAL(clsig1.getHeight(), -1);
    BOOST_CHECK(clsig1.getBlockHash().IsNull());
    BOOST_CHECK(!clsig1.getSig().IsValid());

    // Test parameterized constructor
    int32_t height = 12345;
    uint256 blockHash = GetTestBlockHash(1);
    CBLSSignature sig = CreateRandomBLSSignature();

    ChainLockSig clsig2(height, blockHash, sig);
    BOOST_CHECK(!clsig2.IsNull());
    BOOST_CHECK_EQUAL(clsig2.getHeight(), height);
    BOOST_CHECK(clsig2.getBlockHash() == blockHash);
    BOOST_CHECK(clsig2.getSig() == sig);
}

BOOST_AUTO_TEST_CASE(chainlock_null_test)
{
    ChainLockSig clsig;

    // Default constructed should be null
    BOOST_CHECK(clsig.IsNull());

    // With height set but null hash, should not be null
    clsig = ChainLockSig(100, uint256(), CBLSSignature());
    BOOST_CHECK(!clsig.IsNull());

    // With valid data should not be null
    clsig = CreateChainLock(100, GetTestBlockHash(1));
    BOOST_CHECK(!clsig.IsNull());
}

BOOST_AUTO_TEST_CASE(chainlock_serialization_test)
{
    // Test with valid chainlock
    ChainLockSig clsig = CreateChainLock(67890, GetTestBlockHash(42));

    // Test serialization preserves all fields
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << clsig;

    ChainLockSig deserialized;
    ss >> deserialized;

    BOOST_CHECK_EQUAL(clsig.getHeight(), deserialized.getHeight());
    BOOST_CHECK(clsig.getBlockHash() == deserialized.getBlockHash());
    BOOST_CHECK(clsig.getSig() == deserialized.getSig());
    BOOST_CHECK_EQUAL(clsig.IsNull(), deserialized.IsNull());
}

BOOST_AUTO_TEST_CASE(chainlock_tostring_test)
{
    // Test null chainlock
    ChainLockSig nullClsig;
    std::string nullStr = nullClsig.ToString();
    BOOST_CHECK(!nullStr.empty());

    // Test valid chainlock
    int32_t height = 123456;
    uint256 blockHash = GetTestBlockHash(789);
    ChainLockSig clsig = CreateChainLock(height, blockHash);

    std::string str = clsig.ToString();
    BOOST_CHECK(!str.empty());

    // ToString should contain height and hash info
    BOOST_CHECK(str.find(strprintf("%d", height)) != std::string::npos);
    BOOST_CHECK(str.find(blockHash.ToString().substr(0, 10)) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(chainlock_edge_cases_test)
{
    // Test with edge case heights
    ChainLockSig clsig1 = CreateChainLock(0, GetTestBlockHash(1));
    BOOST_CHECK_EQUAL(clsig1.getHeight(), 0);
    BOOST_CHECK(!clsig1.IsNull());

    ChainLockSig clsig2 = CreateChainLock(std::numeric_limits<int32_t>::max(), GetTestBlockHash(2));
    BOOST_CHECK_EQUAL(clsig2.getHeight(), std::numeric_limits<int32_t>::max());

    // Test serialization with extreme values
    CDataStream ss1(SER_NETWORK, PROTOCOL_VERSION);
    ss1 << clsig1;
    ChainLockSig clsig1_deserialized;
    ss1 >> clsig1_deserialized;
    BOOST_CHECK_EQUAL(clsig1.getHeight(), clsig1_deserialized.getHeight());

    CDataStream ss2(SER_NETWORK, PROTOCOL_VERSION);
    ss2 << clsig2;
    ChainLockSig clsig2_deserialized;
    ss2 >> clsig2_deserialized;
    BOOST_CHECK_EQUAL(clsig2.getHeight(), clsig2_deserialized.getHeight());
}

BOOST_AUTO_TEST_CASE(chainlock_comparison_test)
{
    // Create identical chainlocks
    int32_t height = 5000;
    uint256 blockHash = GetTestBlockHash(10);
    CBLSSignature sig = CreateRandomBLSSignature();

    ChainLockSig clsig1(height, blockHash, sig);
    ChainLockSig clsig2(height, blockHash, sig);

    // Verify getters return same values
    BOOST_CHECK_EQUAL(clsig1.getHeight(), clsig2.getHeight());
    BOOST_CHECK(clsig1.getBlockHash() == clsig2.getBlockHash());
    BOOST_CHECK(clsig1.getSig() == clsig2.getSig());

    // Different chainlocks
    ChainLockSig clsig3(height + 1, blockHash, sig);
    BOOST_CHECK(clsig1.getHeight() != clsig3.getHeight());

    ChainLockSig clsig4(height, GetTestBlockHash(11), sig);
    BOOST_CHECK(clsig1.getBlockHash() != clsig4.getBlockHash());
}

BOOST_AUTO_TEST_CASE(chainlock_malformed_data_test)
{
    // Test deserialization of truncated data
    ChainLockSig clsig = CreateChainLock(1000, GetTestBlockHash(5));

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << clsig;

    // Truncate the stream
    std::string data = ss.str();
    for (size_t truncateAt = 1; truncateAt < data.size(); truncateAt += 10) {
        CDataStream truncated(std::vector<unsigned char>(data.begin(), data.begin() + truncateAt), SER_NETWORK,
                              PROTOCOL_VERSION);

        ChainLockSig deserialized;
        try {
            truncated >> deserialized;
            // If no exception, verify it's either complete or default
            if (truncateAt < sizeof(int32_t)) {
                BOOST_CHECK(deserialized.IsNull());
            }
        } catch (const std::exception&) {
            // Expected for most truncation points
        }
    }
}

BOOST_FIXTURE_TEST_CASE(stale_chainlocks_are_remembered_for_duplicate_suppression, TestingSetup)
{
    m_node.clhandler->CheckActiveState();

    auto best_clsig = CreateChainLock(100, GetTestBlockHash(1));
    BOOST_REQUIRE(m_node.chainlocks->UpdateBestChainlock(::SerializeHash(best_clsig), best_clsig, /*pindex=*/nullptr));

    BOOST_CHECK_EQUAL(m_node.clhandler->SeenChainLockCacheSizeForTesting(), 0U);

    for (uint32_t i = 0; i < 10; ++i) {
        auto stale_clsig = CreateChainLock(100, GetTestBlockHash(1000 + i));
        const auto hash = ::SerializeHash(stale_clsig);

        [[maybe_unused]] const auto result =
            m_node.clhandler->ProcessNewChainLock(/*from=*/0, stale_clsig, *m_node.llmq_ctx->qman, hash);

        BOOST_CHECK(m_node.clhandler->AlreadyHave(CInv{MSG_CLSIG, hash}));
        BOOST_CHECK_EQUAL(m_node.clhandler->SeenChainLockCacheSizeForTesting(), static_cast<size_t>(i) + 1U);
    }
}

BOOST_FIXTURE_TEST_CASE(seen_chainlock_cache_is_bounded, TestingSetup)
{
    m_node.clhandler->CheckActiveState();

    const auto process = [&](size_t i) {
        SetMockTime(std::chrono::seconds{100000 + static_cast<int64_t>(i)});
        auto clsig = CreateChainLock(100, GetTestBlockHash(static_cast<uint32_t>(2000 + i)));
        const auto hash = ::SerializeHash(clsig);
        [[maybe_unused]] const auto result =
            m_node.clhandler->ProcessNewChainLock(/*from=*/-1, clsig, *m_node.llmq_ctx->qman, hash);
        return hash;
    };

    std::vector<uint256> recent_hashes;
    for (size_t i = 0; i <= MAX_SEEN_CHAINLOCKS; ++i) {
        const auto hash = process(i);
        if (i >= MAX_SEEN_CHAINLOCKS + 1 - RECENT_CHAINLOCKS_TO_RETAIN) {
            recent_hashes.emplace_back(hash);
        }
        BOOST_CHECK_LE(m_node.clhandler->SeenChainLockCacheSizeForTesting(), MAX_SEEN_CHAINLOCKS);
    }

    for (const auto& hash : recent_hashes) {
        BOOST_CHECK(m_node.clhandler->AlreadyHave(CInv{MSG_CLSIG, hash}));
    }
    SetMockTime(0s);
}

BOOST_FIXTURE_TEST_CASE(best_chainlock_is_already_have_after_seen_cache_eviction, TestingSetup)
{
    m_node.clhandler->CheckActiveState();

    auto best_clsig = CreateChainLock(100, GetTestBlockHash(1));
    const auto best_hash = ::SerializeHash(best_clsig);
    BOOST_REQUIRE(m_node.chainlocks->UpdateBestChainlock(best_hash, best_clsig, /*pindex=*/nullptr));
    BOOST_CHECK(m_node.clhandler->AlreadyHave(CInv{MSG_CLSIG, best_hash}));

    // Insert enough unique CLSIGs to force at least one prune of the seen cache.
    for (size_t i = 0; i <= MAX_SEEN_CHAINLOCKS; ++i) {
        auto clsig = CreateChainLock(static_cast<int32_t>(101 + i), GetTestBlockHash(static_cast<uint32_t>(3000 + i)));
        [[maybe_unused]] const auto result =
            m_node.clhandler->ProcessNewChainLock(/*from=*/-1, clsig, *m_node.llmq_ctx->qman, ::SerializeHash(clsig));
        BOOST_CHECK_LE(m_node.clhandler->SeenChainLockCacheSizeForTesting(), MAX_SEEN_CHAINLOCKS);
    }

    BOOST_CHECK(m_node.clhandler->AlreadyHave(CInv{MSG_CLSIG, best_hash}));
}

namespace {
//! Regtest spork key matching Params().SporkAddress(), as used by the functional tests.
constexpr const char* REGTEST_SPORK_PRIVKEY{"cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"};
} // namespace

BOOST_FIXTURE_TEST_CASE(coinbase_chainlock_processing, RegTestingSetup)
{
    BOOST_REQUIRE(m_node.sporkman->SetSporkAddress(Params().SporkAddress()));
    BOOST_REQUIRE(m_node.sporkman->SetPrivKey(REGTEST_SPORK_PRIVKEY));
    BOOST_REQUIRE(m_node.sporkman->UpdateSpork(SPORK_19_CHAINLOCKS_ENABLED, 0).has_value());
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());

    std::vector<CBlockIndex> indexes(501);
    std::vector<uint256> hashes(indexes.size());
    for (size_t i = 0; i < indexes.size(); ++i) {
        hashes[i] = GetTestBlockHash(i);
        indexes[i].nHeight = i;
        indexes[i].phashBlock = &hashes[i];
        if (i > 0) indexes[i].pprev = &indexes[i - 1];
        indexes[i].BuildSkip();
    }

    CCbTx cbtx;
    cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    cbtx.bestCLSignature = CreateRandomBLSSignature();
    CMutableTransaction tx;
    tx.nVersion = CTransaction::SPECIAL_VERSION;
    tx.nType = TRANSACTION_COINBASE;
    tx.vin.resize(1);
    CBlock block;
    const auto set_payload = [&] {
        SetTxPayload(tx, cbtx);
        block.vtx = {MakeTransactionRef(tx)};
    };
    const auto process = [&](const CBlockIndex* index) {
        const auto result = m_node.clhandler->ProcessCoinbaseChainLock(block, index, *m_node.llmq_ctx->qman);
        BOOST_CHECK(!result.m_error);
        BOOST_CHECK(result.m_inventory.empty());
        // A structurally valid signature still requires quorum verification.
        BOOST_CHECK(m_node.chainlocks->GetBestChainLock().IsNull());
    };

    // ProcessNewChainLock records the derived signature before verification. No quorum exists in this fixture.
    for (const uint32_t offset : {0U, 5U, 499U}) {
        cbtx.bestCLHeightDiff = offset;
        set_payload();
        process(&indexes.back());
        const int32_t height = 499 - offset;
        const ChainLockSig expected{height, hashes[height], cbtx.bestCLSignature};
        BOOST_CHECK(m_node.clhandler->AlreadyHave(CInv{MSG_CLSIG, ::SerializeHash(expected)}));
    }
    const auto seen = m_node.clhandler->SeenChainLockCacheSizeForTesting();
    BOOST_CHECK_EQUAL(seen, 3U);

    for (const uint32_t offset :
         {500U, 501U, uint32_t{std::numeric_limits<int32_t>::max()}, std::numeric_limits<uint32_t>::max()}) {
        cbtx.bestCLHeightDiff = offset;
        set_payload();
        process(&indexes.back());
    }
    cbtx.bestCLHeightDiff = 1;
    set_payload();
    process(nullptr);
    process(&indexes.front());
    process(&indexes[431]); // Before v20 activation on regtest.

    m_node.mn_sync->Reset(/*fForce=*/true);
    process(&indexes.back());
    m_node.mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE(m_node.sporkman->UpdateSpork(SPORK_19_CHAINLOCKS_ENABLED, 4070908800).has_value());
    process(&indexes.back());
    BOOST_REQUIRE(m_node.sporkman->UpdateSpork(SPORK_19_CHAINLOCKS_ENABLED, 0).has_value());

    cbtx.bestCLSignature = {};
    set_payload();
    process(&indexes.back());
    cbtx.nVersion = CCbTx::Version::MERKLE_ROOT_QUORUMS;
    set_payload();
    process(&indexes.back());
    tx.vExtraPayload = {0xff};
    block.vtx = {MakeTransactionRef(tx)};
    process(&indexes.back());
    tx.nType = TRANSACTION_NORMAL;
    block.vtx = {MakeTransactionRef(tx)};
    process(&indexes.back());
    block.vtx.clear();
    process(&indexes.back());
    BOOST_CHECK_EQUAL(m_node.clhandler->SeenChainLockCacheSizeForTesting(), seen);
}

// A CLSIG is only ever sent in reply to a GETDATA, so one that the peer neither announced nor was
// asked for must be dropped before ProcessNewChainLock -- which would otherwise remember its hash
// and do that work again for every distinct signature blob, at no cost to the sender.
BOOST_FIXTURE_TEST_CASE(unrequested_clsig_is_dropped_and_scored, TestChain100Setup)
{
    LOCK(NetEventsInterface::g_msgproc_mutex);

    // INV announcements for non-spork objects are only tracked outside IBD; the 100 mined blocks
    // of this fixture already take us out of it.
    BOOST_REQUIRE(!m_node.chainman->ActiveChainstate().IsInitialBlockDownload());

    // Every Dash-specific message is offered to CMNAuth first, which asserts a loaded metadata
    // manager. The fixture leaves it unloaded, so initialise an empty cache here.
    BOOST_REQUIRE(m_node.mn_metaman->LoadCache(/*load_cache=*/false));

    // The CLSIG branch in net_processing is gated on spork 19. The test fixture builds a bare
    // CSporkManager, so wire up the regtest signer before setting the spork.
    BOOST_REQUIRE(m_node.sporkman->SetSporkAddress(Params().SporkAddress()));
    BOOST_REQUIRE(m_node.sporkman->SetPrivKey(REGTEST_SPORK_PRIVKEY));
    BOOST_REQUIRE(m_node.sporkman->UpdateSpork(SPORK_19_CHAINLOCKS_ENABLED, 0).has_value());
    BOOST_REQUIRE(m_node.chainlocks->IsEnabled());

    auto unsolicited_peer{MakeTestPeer(/*id=*/41)};
    auto announcing_peer{MakeTestPeer(/*id=*/42)};
    m_node.peerman->InitializeNode(*unsolicited_peer, NODE_NETWORK);
    m_node.peerman->InitializeNode(*announcing_peer, NODE_NETWORK);

    const auto unsolicited_clsig = CreateChainLock(200, GetTestBlockHash(41));
    const CInv unsolicited_inv{MSG_CLSIG, ::SerializeHash(unsolicited_clsig)};

    // Sent twice on purpose. Without the gate the first copy would still be scored (this chain is
    // too short to resolve a signing quorum for height 200) but the second would hit the seen-cache
    // dedup and cost the peer nothing -- so only charging for both proves the gate is what rejected
    // them, and that an unsolicited peer cannot keep repeating the work for free.
    for (int i = 0; i < 2; ++i) {
        CDataStream unsolicited_payload{SER_NETWORK, PROTOCOL_VERSION};
        unsolicited_payload << unsolicited_clsig;
        SendMessage(*m_node.peerman, *unsolicited_peer, NetMsgType::CLSIG, std::move(unsolicited_payload));
    }

    // Never reached ProcessNewChainLock: the hash was not recorded in the seen cache, so the peer
    // could not have displaced a genuine entry, and it was scored for each attempt.
    BOOST_CHECK(!m_node.clhandler->AlreadyHave(unsolicited_inv));
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *unsolicited_peer),
                      2 * UNREQUESTED_OBJECT_MISBEHAVIOR_SCORE);

    // Announcing the CLSIG is NOT enough to authorise it. An INV creates a candidate immediately,
    // but the GETDATA only goes out later from SendMessages, so accepting on the announcement alone
    // would let a peer authorise its own payload by racing INV and payload back to back -- which
    // costs it nothing and defeats the gate entirely.
    const auto announced_clsig = CreateChainLock(201, GetTestBlockHash(42));
    const CInv announced_inv{MSG_CLSIG, ::SerializeHash(announced_clsig)};

    AnnounceInv(*m_node.peerman, *announcing_peer, announced_inv);
    {
        const int score_before_race = MisbehaviorScore(*m_node.peerman, *announcing_peer);
        CDataStream raced_payload{SER_NETWORK, PROTOCOL_VERSION};
        raced_payload << announced_clsig;
        SendMessage(*m_node.peerman, *announcing_peer, NetMsgType::CLSIG, std::move(raced_payload));

        BOOST_CHECK(!m_node.clhandler->AlreadyHave(announced_inv));
        BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *announcing_peer),
                          score_before_race + UNREQUESTED_OBJECT_MISBEHAVIOR_SCORE);
    }

    // Once SendMessages has actually issued the GETDATA the same payload is authorised. The
    // rejection above must not have consumed the candidate, or no GETDATA would go out at all.
    SetMockTime(GetTime<std::chrono::seconds>() + 61s);
    m_node.peerman->SendMessages(announcing_peer.get());
    const int score_before = MisbehaviorScore(*m_node.peerman, *announcing_peer);

    CDataStream announced_payload{SER_NETWORK, PROTOCOL_VERSION};
    announced_payload << announced_clsig;
    SendMessage(*m_node.peerman, *announcing_peer, NetMsgType::CLSIG, std::move(announced_payload));

    BOOST_CHECK(m_node.clhandler->AlreadyHave(announced_inv));
    // Exactly the pre-existing invalid-CLSIG penalty and nothing else. This fixture's chain is 100
    // blocks, so a CLSIG at height 201 resolves to no signing quorum and ProcessNewChainLock scores
    // 10 -- which is what proves the message got past the gate. Asserting the total exactly is what
    // would catch the gate also charging an authorised peer.
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *announcing_peer), score_before + 10);
    // One GETDATA authorises exactly one answer. Both the in-flight request and the late-answer
    // grace are spent, so a replay of the very payload we asked for is unsolicited again -- a peer
    // must not be able to induce one request and then repeat the payload for free.
    const int score_before_replay = MisbehaviorScore(*m_node.peerman, *announcing_peer);
    CDataStream replayed_payload{SER_NETWORK, PROTOCOL_VERSION};
    replayed_payload << announced_clsig;
    SendMessage(*m_node.peerman, *announcing_peer, NetMsgType::CLSIG, std::move(replayed_payload));
    BOOST_CHECK_EQUAL(MisbehaviorScore(*m_node.peerman, *announcing_peer),
                      score_before_replay + UNREQUESTED_OBJECT_MISBEHAVIOR_SCORE);

    m_node.peerman->FinalizeNode(*unsolicited_peer);
    m_node.peerman->FinalizeNode(*announcing_peer);
    SetMockTime(0s);
}

BOOST_AUTO_TEST_SUITE_END()
