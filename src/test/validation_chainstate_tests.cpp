// Copyright (c) 2020-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <bls/bls.h>
#include <bls/bls_batchverifier.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <evo/evodb.h>
#include <evo/deterministicmns.h>
#include <index/txindex.h>
#include <node/interface_ui.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/check.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include <boost/signals2/connection.hpp>
#include <boost/test/unit_test.hpp>

namespace {

class TipEventCounter final : public CValidationInterface
{
public:
    int block_connected{0};
    int updated_tip{0};
    int mn_list_changed{0};
    int chainstate_flushed{0};

    void BlockConnected(const std::shared_ptr<const CBlock>&, const CBlockIndex*) override { ++block_connected; }
    void UpdatedBlockTip(const CBlockIndex*, const CBlockIndex*, bool) override { ++updated_tip; }
    void NotifyMasternodeListChanged(bool, const CDeterministicMNList&, const CDeterministicMNListDiff&) override { ++mn_list_changed; }
    void ChainStateFlushed(const CBlockLocator&) override { ++chainstate_flushed; }
};

//! Runs a probe once, from inside the first ConnectTip that checks a block:
//! after ConnectBlock committed it and before the tip moves.
class BlockCheckedProbe final : public CValidationInterface
{
public:
    std::function<void()> probe;

    void BlockChecked(const CBlock&, const BlockValidationState& state) override
    {
        if (probe && state.IsValid()) std::exchange(probe, nullptr)();
    }
};

//! Whether a thread with no scheme override accepts a fresh signature made
//! under `legacy`, alone and batched.
bool AnotherThreadAccepts(bool legacy)
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    const CBLSPublicKey pk{sk.GetPublicKey()};
    const uint256 msg_hash{InsecureRand256()};
    const CBLSSignature sig{sk.Sign(msg_hash, legacy)};
    bool valid{false}, batch_clean{false};
    std::thread{[&] {
        valid = sig.VerifyInsecure(pk, msg_hash);
        CBLSBatchVerifier<int, uint256> batch_verifier(/*_secureVerification=*/false, /*_perMessageFallback=*/false);
        batch_verifier.PushMessage(/*sourceId=*/0, msg_hash, msg_hash, sig, pk);
        batch_verifier.Verify();
        batch_clean = batch_verifier.badSources.empty();
    }}.join();
    return valid && batch_clean;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(validation_chainstate_tests, ChainTestingSetup)

//! Test resizing coins-related Chainstate caches during runtime.
//!
BOOST_AUTO_TEST_CASE(validation_chainstate_resize_caches)
{
    ChainstateManager& manager = *Assert(m_node.chainman);
    CTxMemPool& mempool = *Assert(m_node.mempool);
    MakeDashChainContexts(/*llmq_dbs_wipe=*/false);
    Chainstate& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool, *m_node.evodb, *Assert(m_node.special_tx),
                                                                            *Assert(m_node.chainlocks), m_node.isman.get()));
    c1.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c1.InitCoinsCache(1 << 23));
    BOOST_REQUIRE(c1.LoadGenesisBlock()); // Need at least one block loaded to be able to flush caches

    // Add a coin to the in-memory cache, upsize once, then downsize.
    {
        LOCK(::cs_main);
        const auto outpoint = AddTestCoin(c1.CoinsTip());

        // Set a meaningless bestblock value in the coinsview cache - otherwise we won't
        // flush during ResizecoinsCaches() and will subsequently hit an assertion.
        c1.CoinsTip().SetBestBlock(InsecureRand256());

        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            1 << 24,  // upsizing the coinsview cache
            1 << 22  // downsizing the coinsdb cache
        );

        // View should still have the coin cached, since we haven't destructed the cache on upsize.
        BOOST_CHECK(c1.CoinsTip().HaveCoinInCache(outpoint));

        c1.ResizeCoinsCaches(
            1 << 22,  // downsizing the coinsview cache
            1 << 23  // upsizing the coinsdb cache
        );

        // The view cache should be empty since we had to destruct to downsize.
        BOOST_CHECK(!c1.CoinsTip().HaveCoinInCache(outpoint));
    }
}

//! Test UpdateTip behavior for both active and background chainstates.
//!
//! When run on the background chainstate, UpdateTip should do a subset
//! of what it does for the active chainstate.
BOOST_FIXTURE_TEST_CASE(chainstate_update_tip, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    uint256 curr_tip = ::g_best_block;

    // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
    // be found.
    mineBlocks(10);

    // After adding some blocks to the tip, best block should have changed.
    BOOST_CHECK(::g_best_block != curr_tip);

    // Grab block 1 from disk; we'll add it to the background chain later.
    std::shared_ptr<CBlock> pblockone = std::make_shared<CBlock>();
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(chainman.m_blockman.ReadBlockFromDisk(*pblockone, *chainman.ActiveChain()[1]));
    }

    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(
        this, NoMalleation, /*reset_chainstate=*/ true));

    // Ensure our active chain is the snapshot chainstate.
    BOOST_CHECK(WITH_LOCK(::cs_main, return chainman.IsSnapshotActive()));

    curr_tip = ::g_best_block;

    // Mine a new block on top of the activated snapshot chainstate.
    mineBlocks(1);  // Defined in TestChain100Setup.

    // After adding some blocks to the snapshot tip, best block should have changed.
    BOOST_CHECK(::g_best_block != curr_tip);

    curr_tip = ::g_best_block;

    BOOST_CHECK_EQUAL(chainman.GetAll().size(), 2);

    Chainstate& background_cs{*Assert([&]() -> Chainstate* {
        for (Chainstate* cs : chainman.GetAll()) {
            if (cs != &chainman.ActiveChainstate()) {
                return cs;
            }
        }
        return nullptr;
    }())};

    // Append the first block to the background chain.
    BlockValidationState state;
    CBlockIndex* pindex = nullptr;
    const CChainParams& chainparams = Params();
    bool newblock = false;

    // TODO: much of this is inlined from ProcessNewBlock(); just reuse PNB()
    // once it is changed to support multiple chainstates.
    {
        LOCK(::cs_main);
        bool checked = CheckBlock(*pblockone, state, chainparams.GetConsensus());
        BOOST_CHECK(checked);
        bool accepted = chainman.AcceptBlock(
            pblockone, state, &pindex, true, nullptr, &newblock);
        BOOST_CHECK(accepted);
    }

    SyncWithValidationInterfaceQueue();
    TipEventCounter event_counter;
    RegisterValidationInterface(&event_counter);
    int ui_mn_list_changed{0};
    auto ui_connection = uiInterface.NotifyMasternodeListChanged_connect(
        [&](const CDeterministicMNList&, const CBlockIndex*) { ++ui_mn_list_changed; });

    // UpdateTip is called here.
    bool block_added = background_cs.ActivateBestChain(state, pblockone);
    WITH_LOCK(::cs_main, background_cs.ForceFlushStateToDisk());
    SyncWithValidationInterfaceQueue();
    ui_connection.disconnect();
    UnregisterValidationInterface(&event_counter);

    // Ensure tip is as expected
    BOOST_CHECK_EQUAL(background_cs.m_chain.Tip()->GetBlockHash(), pblockone->GetHash());

    // g_best_block should be unchanged after adding a block to the background
    // validation chain.
    BOOST_CHECK(block_added);
    BOOST_CHECK_EQUAL(curr_tip, ::g_best_block);
    BOOST_CHECK_EQUAL(event_counter.block_connected, 0);
    BOOST_CHECK_EQUAL(event_counter.updated_tip, 0);
    BOOST_CHECK_EQUAL(event_counter.mn_list_changed, 0);
    BOOST_CHECK_EQUAL(event_counter.chainstate_flushed, 0);
    BOOST_CHECK_EQUAL(ui_mn_list_changed, 0);
}

//! A chain whose V19 activation sits above the assumeutxo height, so the
//! background chainstate validates pre-V19 blocks while the snapshot chainstate
//! is already past the fork.
struct V19AboveSnapshotSetup : public TestChain100Setup {
    static constexpr int V19_HEIGHT{150};
    V19AboveSnapshotSetup() :
        TestChain100Setup{ChainType::REGTEST, {"-testactivationheight=v19@150"}}
    {
    }
};

//! The reverse: the snapshot base is past V19, and the background
//! chainstate's genesis tip is not.
struct V19BelowSnapshotSetup : public TestChain100Setup {
    V19BelowSnapshotSetup() :
        TestChain100Setup{ChainType::REGTEST, {"-testactivationheight=v19@50"}}
    {
    }
};

//! bls::bls_legacy_scheme is process-wide but its correct value is per-block, and
//! ProcessSpecialTxsInBlock only ever switches legacy -> basic. ConnectBlock must
//! therefore establish the scheme its parent left behind instead of inheriting
//! whatever the caller held, or a background chainstate would decode historical
//! pre-V19 BLS data under the active snapshot chainstate's basic scheme.
BOOST_FIXTURE_TEST_CASE(chainstate_connectblock_bls_scheme, V19AboveSnapshotSetup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    BOOST_REQUIRE(bls::bls_legacy_scheme.load());

    // Pretend another chainstate moved the flag past the fork, as an active
    // post-V19 snapshot chainstate does.
    bls::bls_legacy_scheme.store(false);
    mineBlocks(1);
    BOOST_CHECK_MESSAGE(bls::bls_legacy_scheme.load(),
                        "connecting a pre-V19 block must restore the legacy scheme");

    // Reach the assumeutxo height and take a snapshot, then take the snapshot
    // chainstate past V19 so the process-wide flag is basic again.
    mineBlocks(9);
    bls::bls_legacy_scheme.store(false);
    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(this, NoMalleation, /*reset_chainstate=*/true));
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainman.IsSnapshotActive()));
    BOOST_CHECK_MESSAGE(bls::bls_legacy_scheme.load(), "activating a pre-V19 snapshot must establish the legacy scheme");

    // The background chainstate was reset to genesis before activation, so
    // the base MN list was not derivable and no lifecycle marker may have
    // been captured: deriving one would fabricate an empty list and poison
    // the shared list cache for the background chainstate's later
    // re-validation of the base region.
    uint256 stale_hash;
    BOOST_CHECK(!m_node.evodb->ReadSnapshotBaseMNListHash(stale_hash));
    mineBlocks(V19_HEIGHT - WITH_LOCK(::cs_main, return chainman.ActiveHeight()));
    BOOST_REQUIRE(!bls::bls_legacy_scheme.load());

    Chainstate& background_cs{*[&] {
        for (Chainstate* cs : chainman.GetAll()) {
            if (cs != &chainman.ActiveChainstate()) return cs;
        }
        assert(false);
    }()};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return background_cs.m_chain.Height()) < V19_HEIGHT - 1);

    // Give the background chainstate the next (pre-V19) block towards the
    // snapshot base, as if downloaded again: the snapshot setup only dropped its
    // HAVE_DATA flag, not the data.
    const int background_height{WITH_LOCK(::cs_main, return background_cs.m_chain.Height())};
    {
        LOCK(::cs_main);
        CBlockIndex* next{chainman.ActiveChain()[background_height + 1]};
        next->nStatus |= BLOCK_HAVE_DATA;
        background_cs.TryAddBlockIndexCandidate(next);
    }
    BlockValidationState state;

    // Other threads verify ChainLocks and recovered signatures for the active
    // chain without cs_main while the background chainstate connects a block.
    CBLSSecretKey sk;
    sk.MakeNewKey();
    const CBLSPublicKey pk{sk.GetPublicKey()};
    const uint256 msg_hash{InsecureRand256()};
    const CBLSSignature basic_sig{sk.Sign(msg_hash, /*specificLegacyScheme=*/false)};
    bool probed{false}, background_legacy{false}, other_thread_valid{false}, other_thread_batch_clean{false};
    auto probe{std::make_shared<BlockCheckedProbe>()};
    probe->probe = [&] {
        probed = true;
        background_legacy = bls::EffectiveLegacyScheme();
        std::thread{[&] {
            other_thread_valid = basic_sig.VerifyInsecure(pk, msg_hash);
            CBLSBatchVerifier<int, uint256> batch_verifier(/*_secureVerification=*/false, /*_perMessageFallback=*/false);
            batch_verifier.PushMessage(/*sourceId=*/0, msg_hash, msg_hash, basic_sig, pk);
            batch_verifier.Verify();
            other_thread_batch_clean = batch_verifier.badSources.empty();
        }}.join();
    };
    RegisterSharedValidationInterface(probe);
    const bool activated{background_cs.ActivateBestChain(state, nullptr)};
    UnregisterSharedValidationInterface(probe);
    BOOST_REQUIRE(activated);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return background_cs.m_chain.Height()), background_height + 1);

    BOOST_REQUIRE(probed);
    BOOST_CHECK_MESSAGE(background_legacy, "the background chainstate must connect a pre-V19 block under legacy");
    BOOST_CHECK_MESSAGE(other_thread_valid, "a background connect must not change another thread's scheme");
    BOOST_CHECK_MESSAGE(other_thread_batch_clean, "a background connect must not make another thread ban a source");

    // The background chainstate validated a pre-V19 block, but only the active
    // chainstate's tip may define the scheme its consumers see.
    BOOST_CHECK_MESSAGE(!bls::bls_legacy_scheme.load(),
                        "a background connect must not leave the active chain on the legacy scheme");
    BOOST_CHECK(!bls::ScopedLegacySchemeOverride::Active());
}

//! An invalid snapshot hands the active role back to the pre-V19 background
//! chainstate from inside its ConnectTip, under that thread's override. Other
//! threads must get the legacy scheme immediately.
BOOST_FIXTURE_TEST_CASE(invalid_snapshot_fallback_establishes_bls_scheme, V19AboveSnapshotSetup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    mineBlocks(10);
    Chainstate& background_cs{chainman.ActiveChainstate()};
    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(this));
    mineBlocks(V19_HEIGHT - WITH_LOCK(::cs_main, return chainman.ActiveHeight()));
    BOOST_REQUIRE(!bls::bls_legacy_scheme.load());

    bool shut_down{false};
    {
        LOCK(::cs_main);
        Coin extra_coin;
        extra_coin.out.nValue = 1 * COIN;
        extra_coin.out.scriptPubKey = CScript{} << OP_TRUE;
        extra_coin.nHeight = 1;
        background_cs.CoinsTip().AddCoin(COutPoint{InsecureRand256(), 0}, std::move(extra_coin), /*possible_overwrite=*/false);
        const bls::ScopedLegacySchemeOverride background_connect_tip{/*legacy=*/true};
        BOOST_REQUIRE(chainman.MaybeCompleteSnapshotValidation([&](bilingual_str) { shut_down = true; }) ==
                      SnapshotCompletionResult::HASH_MISMATCH);
        BOOST_REQUIRE(&chainman.ActiveChainstate() == &background_cs);
    }
    BOOST_REQUIRE(shut_down);
    BOOST_CHECK(bls::bls_legacy_scheme.load());
    BOOST_CHECK(AnotherThreadAccepts(/*legacy=*/true));
}

//! Activating a post-V19 snapshot on a node still on the legacy scheme must
//! switch other threads to basic immediately.
BOOST_FIXTURE_TEST_CASE(snapshot_activation_establishes_bls_scheme, V19BelowSnapshotSetup)
{
    mineBlocks(10);
    bls::bls_legacy_scheme.store(true);
    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(this, NoMalleation, /*reset_chainstate=*/true));
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.chainman->IsSnapshotActive()));
    BOOST_CHECK(!bls::bls_legacy_scheme.load());
    BOOST_CHECK(AnotherThreadAccepts(/*legacy=*/false));
}

BOOST_AUTO_TEST_SUITE_END()
