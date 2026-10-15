// Copyright (c) 2020-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <evo/specialtxman.h>
#include <index/txindex.h>
#include <init.h>
#include <llmq/blockprocessor.h>
#include <llmq/context.h>
#include <node/blockstorage.h>
#include <node/interface_ui.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/coins.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>

#include <vector>

#include <boost/signals2/connection.hpp>
#include <boost/test/unit_test.hpp>

namespace {

class TipEventCounter final : public CValidationInterface
{
public:
    explicit TipEventCounter(const CEvoDB& evodb) :
        m_evodb{evodb}
    {
    }

    int block_connected{0};
    int updated_tip{0};
    int mn_list_changed{0};
    int chainstate_flushed{0};
    //! The Evo view each BlockChecked callback ran under.
    std::vector<EvoDbIdentity> block_checked_views;

    void BlockChecked(const CBlock&, const BlockValidationState&) override
    {
        block_checked_views.push_back(m_evodb.CurrentView().identity);
    }
    void BlockConnected(const std::shared_ptr<const CBlock>&, const CBlockIndex*) override { ++block_connected; }
    void UpdatedBlockTip(const CBlockIndex*, const CBlockIndex*, bool) override { ++updated_tip; }
    void NotifyMasternodeListChanged(bool, const CDeterministicMNList&, const CDeterministicMNListDiff&) override { ++mn_list_changed; }
    void ChainStateFlushed(const CBlockLocator&) override { ++chainstate_flushed; }

private:
    const CEvoDB& m_evodb;
};

//! Whether `key` is durably stored in the snapshot generation `generation`.
template <typename K>
bool StoredInGeneration(CEvoDB& evodb, uint32_t generation, const K& key)
{
    DataStream raw_key{CEvoDBStore::GenerationPrefix(generation)};
    raw_key << key;
    return evodb.GetRawDB().Exists(raw_key);
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
        BOOST_REQUIRE(node::ReadBlockFromDisk(*pblockone, chainman.ActiveChain()[1], chainman.GetConsensus()));
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
    TipEventCounter event_counter{*m_node.evodb};
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
    // Synchronous notifications of a background connect run under its view.
    BOOST_CHECK(event_counter.block_checked_views == std::vector<EvoDbIdentity>{EvoDbIdentity::NORMAL});

    // A queued event runs under the view it was enqueued with, and is dropped
    // when that view's snapshot generation was retired in the meantime.
    CEvoDB& evodb{*m_node.evodb};
    TipEventCounter queued_counter{evodb};
    RegisterValidationInterface(&queued_counter);
    const EvoViewToken snapshot_view{evodb.ViewFor(EvoDbIdentity::SNAPSHOT)};
    EvoViewToken retired_view{snapshot_view};
    ++retired_view.generation;
    {
        EvoViewScope live{snapshot_view};
        GetMainSignals().ChainStateFlushed(CBlockLocator{});
    }
    {
        EvoViewScope stale{retired_view};
        GetMainSignals().ChainStateFlushed(CBlockLocator{});
    }
    SyncWithValidationInterfaceQueue();
    UnregisterValidationInterface(&queued_counter);
    BOOST_CHECK_EQUAL(queued_counter.chainstate_flushed, 1);

    // Scheduler tasks start before the block databases load, and a load retry
    // replaces the Evo database: they bind the active view only once startup
    // has published the database it keeps.
    bool scheduler_bound{true};
    RunSchedulerTask(m_node, [&] { scheduler_bound = evodb.IsBound(); });
    BOOST_CHECK(!scheduler_bound);
    m_node.evodb_for_dispatch = &evodb;
    RunSchedulerTask(m_node, [&] { scheduler_bound = evodb.IsBound(); });
    BOOST_CHECK(scheduler_bound);
    m_node.evodb_for_dispatch = nullptr;

    // Recomputing the EHF signals at the active snapshot tip runs and flushes
    // only the snapshot view, leaving the background's pending state alone.
    const auto normal_key{std::make_pair(std::string{"test_force_signal"}, uint8_t{0})};
    const auto snapshot_key{std::make_pair(std::string{"test_force_signal"}, uint8_t{1})};
    {
        LOCK(::cs_main);
        {
            auto tx = evodb.BeginTransaction(EvoDbIdentity::NORMAL);
            evodb.Write(normal_key, uint8_t{1});
            tx->Commit();
        }
        {
            auto tx = evodb.BeginTransaction(EvoDbIdentity::SNAPSHOT);
            evodb.Write(snapshot_key, uint8_t{1});
            tx->Commit();
        }
        Chainstate& active{chainman.ActiveChainstate()};
        BOOST_REQUIRE(m_node.special_tx->m_mnhfman->ForceSignalDBUpdate(active.EvoView(), active.m_chain.Tip()));
        BOOST_CHECK(StoredInGeneration(evodb, snapshot_view.generation, snapshot_key));
        BOOST_CHECK(!evodb.GetRawDB().Exists(normal_key));
        BOOST_REQUIRE(evodb.CommitRootTransaction(EvoDbIdentity::NORMAL));
    }

    // During dual operation an unbound Evo read is a typed local error, never
    // a silent pick of either view: for the background chainstate's state and
    // for the active one's alike. The check is not an Assume, so it fires in
    // every build.
    BOOST_CHECK(!evodb.IsBound());
    BOOST_CHECK_THROW(m_node.dmnman->GetListForBlock(WITH_LOCK(::cs_main, return background_cs.m_chain.Tip())),
                      EvoViewBindingError);
    BOOST_CHECK_THROW(m_node.dmnman->GetListAtChainTip(), EvoViewBindingError);

    // A connect-path entry point taking an explicit chainstate refuses to run
    // under another chainstate's view with the same typed binding error as the
    // undo path, which every chainstate boundary classifies as a local stall.
    {
        LOCK(::cs_main);
        EvoViewScope snapshot_bound{chainman.ActiveChainstate().EvoView()};
        const CBlockIndex* background_tip{background_cs.m_chain.Tip()};
        CBlock block;
        BOOST_REQUIRE(node::ReadBlockFromDisk(block, background_tip, chainman.GetConsensus()));
        BlockValidationState wrong_view_state;
        BOOST_CHECK_THROW(m_node.llmq_ctx->quorum_block_processor->ProcessBlock(background_cs, block, background_tip,
                                                                                wrong_view_state, /*fJustCheck=*/true,
                                                                                /*fBLSChecks=*/false),
                          EvoViewBindingError);
        CCoinsViewCache coins{&background_cs.CoinsTip()};
        MNListUpdates updates;
        BOOST_CHECK_THROW(m_node.special_tx->ProcessSpecialTxsInBlock(
                              background_cs, background_cs.m_chain, block, background_tip,
                              GetSpecialTxRules(background_tip->pprev, chainman), coins,
                              GetBlockSubsidy(background_tip, chainman.GetConsensus()), /*fJustCheck=*/true,
                              /*fCheckCbTxMerkleRoots=*/false, wrong_view_state, updates),
                          EvoViewBindingError);
        BOOST_CHECK(wrong_view_state.IsValid());
    }
}

//! A chain whose V19 activation sits above the assumeutxo height, so the
//! background chainstate validates pre-V19 blocks while the snapshot chainstate
//! is already past the fork.
struct V19AboveSnapshotSetup : public TestChain100Setup {
    static constexpr int V19_HEIGHT{150};
    V19AboveSnapshotSetup() :
        TestChain100Setup{CBaseChainParams::REGTEST, {"-testactivationheight=v19@150"}}
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
    BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(this, NoMalleation, /*reset_chainstate=*/true));
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return chainman.IsSnapshotActive()));

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

    std::vector<CMutableTransaction> noTxns;
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    auto pblock = std::make_shared<const CBlock>(this->CreateBlock(noTxns, scriptPubKey, background_cs));
    BlockValidationState state;
    {
        LOCK(::cs_main);
        CBlockIndex* pindex = nullptr;
        bool newblock = false;
        BOOST_REQUIRE(CheckBlock(*pblock, state, Params().GetConsensus()));
        BOOST_REQUIRE(m_node.chainman->AcceptBlock(pblock, state, &pindex, true, nullptr, &newblock));
    }
    BOOST_REQUIRE(background_cs.ActivateBestChain(state, pblock));

    // The background chainstate validated a pre-V19 block, but only the active
    // chainstate's tip may define the scheme its consumers see.
    BOOST_CHECK_MESSAGE(!bls::bls_legacy_scheme.load(),
                        "a background connect must not leave the active chain on the legacy scheme");
}

BOOST_AUTO_TEST_SUITE_END()
