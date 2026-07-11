// Copyright (c) 2019-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <chainparams.h>
#include <consensus/validation.h>
#include <index/txindex.h>
#include <node/chainstate.h>
#include <node/utxo_snapshot.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/index.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <timedata.h>
#include <uint256.h>
#include <validation.h>
#include <validationinterface.h>

#include <chainlock/handler.h>
#include <evo/evodb.h>
#include <governance/governance.h>
#include <llmq/blockprocessor.h>
#include <llmq/signing.h>

#include <tinyformat.h>

#include <vector>

#include <boost/test/unit_test.hpp>

using node::SnapshotMetadata;

namespace {

void SeedSnapshotMarker(CEvoDB& evodb, const uint256& hash)
{
    auto tx = evodb.BeginTransaction(EvoDbIdentity::SNAPSHOT);
    evodb.WriteBestBlock(EvoDbIdentity::SNAPSHOT, hash);
    tx->Commit();
    BOOST_REQUIRE(evodb.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(validation_chainstatemanager_tests, ChainTestingSetup)

//! Basic tests for ChainstateManager.
//!
//! First create a legacy (IBD) chainstate, then create a snapshot chainstate.
BOOST_AUTO_TEST_CASE(chainstatemanager)
{
    ChainstateManager& manager = *m_node.chainman;
    CTxMemPool& mempool = *m_node.mempool;
    CEvoDB& evodb = *m_node.evodb;
    std::vector<Chainstate*> chainstates;

    BOOST_CHECK(!manager.SnapshotBlockhash().has_value());

    // Create a legacy (IBD) chainstate.
    //
    Chainstate& c1 = WITH_LOCK(::cs_main, return manager.InitializeChainstate(&mempool, evodb, m_node.chain_helper));
    chainstates.push_back(&c1);
    c1.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c1.InitCoinsCache(1 << 23));

    DashChainstateSetup(manager, m_node, /*llmq_dbs_in_memory=*/true, /*llmq_dbs_wipe=*/false);

    BOOST_CHECK(!manager.IsSnapshotActive());
    BOOST_CHECK(WITH_LOCK(::cs_main, return !manager.IsSnapshotValidated()));
    auto all = manager.GetAll();
    BOOST_CHECK_EQUAL_COLLECTIONS(all.begin(), all.end(), chainstates.begin(), chainstates.end());

    auto& active_chain = WITH_LOCK(manager.GetMutex(), return manager.ActiveChain());
    BOOST_CHECK_EQUAL(&active_chain, &c1.m_chain);

    BOOST_CHECK_EQUAL(WITH_LOCK(manager.GetMutex(), return manager.ActiveHeight()), -1);

    auto active_tip = WITH_LOCK(manager.GetMutex(), return manager.ActiveTip());
    auto exp_tip = c1.m_chain.Tip();
    BOOST_CHECK_EQUAL(active_tip, exp_tip);

    BOOST_CHECK(!manager.SnapshotBlockhash().has_value());

    if (m_node.clhandler) {
        m_node.clhandler->Stop();
    }
    DashChainstateSetupClose(m_node);

    // Create a snapshot-based chainstate.
    //
    const uint256 snapshot_blockhash = GetRandHash();
    SeedSnapshotMarker(evodb, snapshot_blockhash);
    Chainstate* c2_ptr = WITH_LOCK(::cs_main, return manager.ActivateExistingSnapshot(
        &mempool, snapshot_blockhash));
    BOOST_REQUIRE(c2_ptr);
    Chainstate& c2 = *c2_ptr;
    chainstates.push_back(&c2);

    DashChainstateSetup(manager, m_node, /*llmq_dbs_in_memory=*/true, /*llmq_dbs_wipe=*/false);

    BOOST_CHECK_EQUAL(manager.SnapshotBlockhash().value(), snapshot_blockhash);

    c2.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c2.InitCoinsCache(1 << 23));
    // Unlike c1, which doesn't have any blocks. Gets us different tip, height.
    c2.LoadGenesisBlock();
    BlockValidationState dummy_state;
    BOOST_CHECK(c2.ActivateBestChain(dummy_state, nullptr));

    BOOST_CHECK(manager.IsSnapshotActive());
    BOOST_CHECK(WITH_LOCK(::cs_main, return !manager.IsSnapshotValidated()));
    BOOST_CHECK_EQUAL(&c2, &manager.ActiveChainstate());
    BOOST_CHECK(&c1 != &manager.ActiveChainstate());
    auto all2 = manager.GetAll();
    BOOST_CHECK_EQUAL_COLLECTIONS(all2.begin(), all2.end(), chainstates.begin(), chainstates.end());

    auto& active_chain2 = WITH_LOCK(manager.GetMutex(), return manager.ActiveChain());
    BOOST_CHECK_EQUAL(&active_chain2, &c2.m_chain);

    BOOST_CHECK_EQUAL(WITH_LOCK(manager.GetMutex(), return manager.ActiveHeight()), 0);

    auto active_tip2 = WITH_LOCK(manager.GetMutex(), return manager.ActiveTip());
    auto exp_tip2 = c2.m_chain.Tip();
    BOOST_CHECK_EQUAL(active_tip2, exp_tip2);

    // Ensure that these pointers actually correspond to different
    // CCoinsViewCache instances.
    BOOST_CHECK(exp_tip != exp_tip2);

    // Let scheduler events finish running to avoid accessing memory that is going to be unloaded
    SyncWithValidationInterfaceQueue();

    if (m_node.clhandler) {
        m_node.clhandler->Stop();
    }
    DashChainstateSetupClose(m_node);
}

//! Test rebalancing the caches associated with each chainstate.
BOOST_AUTO_TEST_CASE(chainstatemanager_rebalance_caches)
{
    ChainstateManager& manager = *m_node.chainman;
    CTxMemPool& mempool = *m_node.mempool;
    CEvoDB& evodb = *m_node.evodb;
    size_t max_cache = 10000;
    manager.m_total_coinsdb_cache = max_cache;
    manager.m_total_coinstip_cache = max_cache;

    std::vector<Chainstate*> chainstates;

    // Create a legacy (IBD) chainstate.
    //
    Chainstate& c1 = WITH_LOCK(cs_main, return manager.InitializeChainstate(&mempool, evodb, m_node.chain_helper));
    chainstates.push_back(&c1);
    c1.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);

    {
        LOCK(::cs_main);
        c1.InitCoinsCache(1 << 23);
        BOOST_REQUIRE(c1.LoadGenesisBlock());
        c1.CoinsTip().SetBestBlock(InsecureRand256());
        manager.MaybeRebalanceCaches();
    }

    BOOST_CHECK_EQUAL(c1.m_coinstip_cache_size_bytes, max_cache);
    BOOST_CHECK_EQUAL(c1.m_coinsdb_cache_size_bytes, max_cache);

    // Create a snapshot-based chainstate.
    //
    const uint256 snapshot_blockhash = GetRandHash();
    SeedSnapshotMarker(evodb, snapshot_blockhash);
    Chainstate* c2_ptr = WITH_LOCK(cs_main, return manager.ActivateExistingSnapshot(&mempool, snapshot_blockhash));
    BOOST_REQUIRE(c2_ptr);
    Chainstate& c2 = *c2_ptr;
    chainstates.push_back(&c2);
    c2.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);

    {
        LOCK(::cs_main);
        c2.InitCoinsCache(1 << 23);
        BOOST_REQUIRE(c2.LoadGenesisBlock());
        c2.CoinsTip().SetBestBlock(InsecureRand256());
        manager.MaybeRebalanceCaches();
    }

    // Since both chainstates are considered to be in initial block download,
    // the snapshot chainstate should take priority.
    BOOST_CHECK_CLOSE(c1.m_coinstip_cache_size_bytes, max_cache * 0.05, 1);
    BOOST_CHECK_CLOSE(c1.m_coinsdb_cache_size_bytes, max_cache * 0.05, 1);
    BOOST_CHECK_CLOSE(c2.m_coinstip_cache_size_bytes, max_cache * 0.95, 1);
    BOOST_CHECK_CLOSE(c2.m_coinsdb_cache_size_bytes, max_cache * 0.95, 1);
}

struct SnapshotTestSetup : TestChain100Setup {
    // Run with coinsdb on the filesystem to support, e.g., moving invalidated
    // chainstate dirs to "*_invalid".
    //
    // Note that this means the tests run considerably slower than in-memory DB
    // tests, but we can't otherwise test this functionality since it relies on
    // destructive filesystem operations.
    SnapshotTestSetup() : TestChain100Setup{
                              CBaseChainParams::REGTEST,
                              {},
                              /*coins_db_in_memory=*/false,
                              /*block_tree_db_in_memory=*/false,
                              /*dash_dbs_in_memory=*/false,
                          }
    {
    }

    std::tuple<Chainstate*, Chainstate*> SetupSnapshot()
    {
        ChainstateManager& chainman = *Assert(m_node.chainman);

        BOOST_CHECK(!chainman.IsSnapshotActive());

        {
            LOCK(::cs_main);
            BOOST_CHECK(!chainman.IsSnapshotValidated());
            BOOST_CHECK(!node::FindSnapshotChainstateDir());
        }

        size_t initial_size;
        size_t initial_total_coins{100};

        // Make some initial assertions about the contents of the chainstate.
        {
            LOCK(::cs_main);
            CCoinsViewCache& ibd_coinscache = chainman.ActiveChainstate().CoinsTip();
            initial_size = ibd_coinscache.GetCacheSize();
            size_t total_coins{0};

            for (CTransactionRef& txn : m_coinbase_txns) {
                COutPoint op{txn->GetHash(), 0};
                BOOST_CHECK(ibd_coinscache.HaveCoin(op));
                total_coins++;
            }

            BOOST_CHECK_EQUAL(total_coins, initial_total_coins);
            BOOST_CHECK_EQUAL(initial_size, initial_total_coins);
        }

        Chainstate& validation_chainstate = chainman.ActiveChainstate();

        // Snapshot should refuse to load at this height.
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(this));
        BOOST_CHECK(!chainman.ActiveChainstate().m_from_snapshot_blockhash);
        BOOST_CHECK(!chainman.SnapshotBlockhash());

        // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
        // be found.
        constexpr int snapshot_height = 110;
        mineBlocks(10);
        initial_size += 10;
        initial_total_coins += 10;

        // Should not load malleated snapshots
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // A UTXO is missing but count is correct
                metadata.m_coins_count -= 1;

                COutPoint outpoint;
                Coin coin;

                auto_infile >> outpoint;
                auto_infile >> coin;
        }));

        BOOST_CHECK(!node::FindSnapshotChainstateDir());

        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Coins count is larger than coins in file
                metadata.m_coins_count += 1;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Coins count is smaller than coins in file
                metadata.m_coins_count -= 1;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Wrong hash
                metadata.m_base_blockhash = uint256::ZERO;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Wrong hash
                metadata.m_base_blockhash = uint256::ONE;
        }));

        BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(this));
        BOOST_CHECK(fs::exists(*node::FindSnapshotChainstateDir()));

        // Ensure our active chain is the snapshot chainstate.
        BOOST_CHECK(!chainman.ActiveChainstate().m_from_snapshot_blockhash->IsNull());
        BOOST_CHECK_EQUAL(
            *chainman.ActiveChainstate().m_from_snapshot_blockhash,
            *chainman.SnapshotBlockhash());

        Chainstate& snapshot_chainstate = chainman.ActiveChainstate();

        // To be checked against later when we try loading a subsequent snapshot.
        uint256 loaded_snapshot_blockhash{*chainman.SnapshotBlockhash()};

        BOOST_CHECK(m_node.evodb->VerifyBestBlock(
            EvoDbIdentity::SNAPSHOT, loaded_snapshot_blockhash));
        BOOST_CHECK(m_node.evodb->VerifyBestBlock(
            EvoDbIdentity::NORMAL, loaded_snapshot_blockhash));
        BOOST_CHECK(m_node.evodb->HasDualChainstateMarker());

        {
            LOCK(::cs_main);

            fs::path found = *node::FindSnapshotChainstateDir();

            // Note: WriteSnapshotBaseBlockhash() is implicitly tested above.
            BOOST_CHECK_EQUAL(
                *node::ReadSnapshotBaseBlockhash(found),
                *chainman.SnapshotBlockhash());

            // Ensure that the genesis block was not marked assumed-valid.
            BOOST_CHECK(!chainman.ActiveChain().Genesis()->IsAssumedValid());
        }

        const AssumeutxoData& au_data = *ExpectedAssumeutxo(snapshot_height, ::Params());
        const CBlockIndex* tip = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveTip());

        BOOST_CHECK_EQUAL(tip->nChainTx, au_data.nChainTx);

        // Make some assertions about the both chainstates. These checks ensure the
        // legacy chainstate hasn't changed and that the newly created chainstate
        // reflects the expected content.
        {
            LOCK(::cs_main);
            int chains_tested{0};

            for (Chainstate* chainstate : chainman.GetAll()) {
                BOOST_TEST_MESSAGE("Checking coins in " << chainstate->ToString());
                CCoinsViewCache& coinscache = chainstate->CoinsTip();

                // Both caches will be empty initially.
                BOOST_CHECK_EQUAL((unsigned int)0, coinscache.GetCacheSize());

                size_t total_coins{0};

                for (CTransactionRef& txn : m_coinbase_txns) {
                    COutPoint op{txn->GetHash(), 0};
                    BOOST_CHECK(coinscache.HaveCoin(op));
                    total_coins++;
                }

                BOOST_CHECK_EQUAL(initial_size , coinscache.GetCacheSize());
                BOOST_CHECK_EQUAL(total_coins, initial_total_coins);
                chains_tested++;
            }

            BOOST_CHECK_EQUAL(chains_tested, 2);
        }

        // Mine some new blocks on top of the activated snapshot chainstate.
        constexpr size_t new_coins{100};
        mineBlocks(new_coins);  // Defined in TestChain100Setup.

        const uint256 snapshot_tip = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveTip()->GetBlockHash());
        BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::SNAPSHOT, snapshot_tip));
        BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, loaded_snapshot_blockhash));

        {
            LOCK(::cs_main);
            size_t coins_in_active{0};
            size_t coins_in_background{0};
            size_t coins_missing_from_background{0};

            for (Chainstate* chainstate : chainman.GetAll()) {
                BOOST_TEST_MESSAGE("Checking coins in " << chainstate->ToString());
                CCoinsViewCache& coinscache = chainstate->CoinsTip();
                bool is_background = chainstate != &chainman.ActiveChainstate();

                for (CTransactionRef& txn : m_coinbase_txns) {
                    COutPoint op{txn->GetHash(), 0};
                    if (coinscache.HaveCoin(op)) {
                        (is_background ? coins_in_background : coins_in_active)++;
                    } else if (is_background) {
                        coins_missing_from_background++;
                    }
                }
            }

            BOOST_CHECK_EQUAL(coins_in_active, initial_total_coins + new_coins);
            BOOST_CHECK_EQUAL(coins_in_background, initial_total_coins);
            BOOST_CHECK_EQUAL(coins_missing_from_background, new_coins);
        }

        // Snapshot should refuse to load after one has already loaded.
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(this));

        // Snapshot blockhash should be unchanged.
        BOOST_CHECK_EQUAL(
            *chainman.ActiveChainstate().m_from_snapshot_blockhash,
            loaded_snapshot_blockhash);
        return std::make_tuple(&validation_chainstate, &snapshot_chainstate);
    }

    // Simulate a restart of the node by flushing all state to disk, clearing the
    // existing ChainstateManager, and unloading the block index.
    //
    // @returns a reference to the "restarted" ChainstateManager
    ChainstateManager& SimulateNodeRestart()
    {
        ChainstateManager& chainman = *Assert(m_node.chainman);

        BOOST_TEST_MESSAGE("Simulating node restart");
        // TestChainSetup owns a txindex connected to the old chainstate. Stop it
        // before destroying that chainstate, just as TestChainSetup's destructor does.
        IndexWaitSynced(*g_txindex);
        g_txindex->Interrupt();
        g_txindex->Stop();
        SyncWithValidationInterfaceQueue();
        g_txindex.reset();
        {
            LOCK(::cs_main);
            for (Chainstate* cs : chainman.GetAll()) {
                cs->ForceFlushStateToDisk();
            }
            // Tear down Dash managers connected to the mempool and old chainstate
            // before LoadVerifyActivateChainstate() recreates them below.
            m_node.govman.reset();
            DashChainstateSetupClose(m_node);
            chainman.ResetChainstates();
            BOOST_CHECK_EQUAL(chainman.GetAll().size(), 0);
            const ChainstateManager::Options chainman_opts{
                .chainparams = ::Params(),
                .adjusted_time_callback = GetAdjustedTime,
            };
            // For robustness, ensure the old manager is destroyed before creating a
            // new one.
            m_node.chainman.reset();
            m_node.chainman.reset(new ChainstateManager(chainman_opts));
        }
        return *Assert(m_node.chainman);
    }
};

struct SnapshotReindexTestSetup : ChainTestingSetup {
    const bool m_old_reindex{node::fReindex};

    SnapshotReindexTestSetup() :
        ChainTestingSetup{CBaseChainParams::REGTEST, {}, /*dash_dbs_in_memory=*/false}
    {
        m_coins_db_in_memory = false;
        m_block_tree_db_in_memory = false;
    }

    ~SnapshotReindexTestSetup() { node::fReindex = m_old_reindex; }

    void CheckSnapshotReindexLoad(bool reindex, bool reindex_chainstate)
    {
        ChainstateManager& chainman = *Assert(m_node.chainman);
        const uint256 base_blockhash = GetRandHash();
        {
            LOCK(::cs_main);
            chainman.InitializeChainstate(m_node.mempool.get(), *m_node.evodb, m_node.chain_helper);
            SeedSnapshotMarker(*m_node.evodb, base_blockhash);
            Chainstate* snapshot_chainstate = chainman.ActivateExistingSnapshot(m_node.mempool.get(), base_blockhash);
            BOOST_REQUIRE(snapshot_chainstate);
            snapshot_chainstate->InitCoinsDB(
                /*cache_size_bytes=*/1 << 20, /*in_memory=*/false, /*should_wipe=*/true);
            BOOST_REQUIRE(node::WriteSnapshotBaseBlockhash(*snapshot_chainstate));
            chainman.ResetChainstates();
        }
        BOOST_REQUIRE(node::FindSnapshotChainstateDir());

        node::ChainstateLoadOptions options;
        options.mempool = Assert(m_node.mempool.get());
        options.block_tree_db_in_memory = m_block_tree_db_in_memory;
        options.coins_db_in_memory = m_coins_db_in_memory;
        options.dash_dbs_in_memory = m_dash_dbs_in_memory;
        options.reindex = reindex;
        options.reindex_chainstate = reindex_chainstate;
        options.bls_threads = llmq::DEFAULT_BLSCHECK_THREADS;
        options.worker_count = llmq::DEFAULT_WORKER_COUNT;
        options.max_recsigs_age = llmq::DEFAULT_MAX_RECOVERED_SIGS_AGE;

        const auto [status, error] = node::LoadChainstate(chainman, *Assert(m_node.mn_metaman.get()),
                                                          *Assert(m_node.sporkman.get()), *Assert(m_node.chainlocks.get()),
                                                          *Assert(m_node.mn_sync.get()), m_node.chain_helper,
                                                          m_node.dmnman, m_node.evodb, m_node.llmq_ctx,
                                                          Assert(m_node.args)->GetDataDirNet(), m_cache_sizes, options);
        BOOST_REQUIRE_MESSAGE(status == node::ChainstateLoadStatus::SUCCESS, error.original);
        BOOST_CHECK(!node::FindSnapshotChainstateDir());
        BOOST_CHECK(!chainman.IsSnapshotActive());
        BOOST_CHECK_EQUAL(chainman.GetAll().size(), 1);
        BOOST_CHECK(!chainman.ActiveChainstate().m_from_snapshot_blockhash);

        uint256 snapshot_marker;
        BOOST_CHECK(!m_node.evodb->ReadBestBlock(EvoDbIdentity::SNAPSHOT, snapshot_marker));

        DashChainstateSetupClose(m_node);
        WITH_LOCK(::cs_main, chainman.ResetChainstates());
    }
};

//! Test basic snapshot activation.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_activate_snapshot, SnapshotTestSetup)
{
    this->SetupSnapshot();
}

//! Test LoadBlockIndex behavior when multiple chainstates are in use.
//!
//! - First, verify that setBlockIndexCandidates is as expected when using a single,
//!   fully-validating chainstate.
//!
//! - Then mark a region of the chain BLOCK_ASSUMED_VALID and introduce a second chainstate
//!   that will tolerate assumed-valid blocks. Run LoadBlockIndex() and ensure that the first
//!   chainstate only contains fully validated blocks and the other chainstate contains all blocks,
//!   even those assumed-valid.
//!
BOOST_FIXTURE_TEST_CASE(chainstatemanager_loadblockindex, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    CTxMemPool& mempool = *m_node.mempool;
    Chainstate& cs1 = chainman.ActiveChainstate();

    int num_indexes{0};
    int num_assumed_valid{0};
    const int expected_assumed_valid{20};
    const int last_assumed_valid_idx{40};
    const int assumed_valid_start_idx = last_assumed_valid_idx - expected_assumed_valid;

    CBlockIndex* validated_tip{nullptr};
    CBlockIndex* assumed_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};

    auto reload_all_block_indexes = [&]() {
        for (Chainstate* cs : chainman.GetAll()) {
            LOCK(::cs_main);
            cs->UnloadBlockIndex();
            BOOST_CHECK(cs->setBlockIndexCandidates.empty());
        }

        WITH_LOCK(::cs_main, chainman.LoadBlockIndex());
    };

    // Ensure that without any assumed-valid BlockIndex entries, all entries are considered
    // tip candidates.
    reload_all_block_indexes();
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.size(), cs1.m_chain.Height() + 1);

    // Mark some region of the chain assumed-valid.
    for (int i = 0; i <= cs1.m_chain.Height(); ++i) {
        LOCK(::cs_main);
        auto index = cs1.m_chain[i];

        if (i < last_assumed_valid_idx && i >= assumed_valid_start_idx) {
            index->nStatus = BlockStatus::BLOCK_VALID_TREE | BlockStatus::BLOCK_ASSUMED_VALID;
        }

        ++num_indexes;
        if (index->IsAssumedValid()) ++num_assumed_valid;

        // Note the last fully-validated block as the expected validated tip.
        if (i == (assumed_valid_start_idx - 1)) {
            validated_tip = index;
            BOOST_CHECK(!index->IsAssumedValid());
        }
    }

    BOOST_CHECK_EQUAL(expected_assumed_valid, num_assumed_valid);

    const uint256 snapshot_blockhash = GetRandHash();
    SeedSnapshotMarker(*m_node.evodb, snapshot_blockhash);
    Chainstate* cs2_ptr = WITH_LOCK(::cs_main,
        return chainman.ActivateExistingSnapshot(&mempool, snapshot_blockhash));
    BOOST_REQUIRE(cs2_ptr);
    Chainstate& cs2 = *cs2_ptr;

    reload_all_block_indexes();

    // The fully validated chain only has candidates up to the start of the assumed-valid
    // blocks.
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.count(validated_tip), 1);
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.count(assumed_tip), 0);
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.size(), assumed_valid_start_idx);

    // The assumed-valid tolerant chain has all blocks as candidates.
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.count(validated_tip), 1);
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.count(assumed_tip), 1);
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.size(), num_indexes);

    // -reindex-chainstate removes the snapshot before rebuilding coins. Its
    // block-index load must therefore clear snapshot-only metadata before
    // enforcing the single-chainstate invariants.
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(chainman.DeleteSnapshotChainstate());
        cs1.UnloadBlockIndex();
        BOOST_REQUIRE(chainman.LoadBlockIndex(/*reset_assumed_valid=*/true));
        BOOST_CHECK_EQUAL(chainman.GetAll().size(), 1);
        for (int i = assumed_valid_start_idx; i < last_assumed_valid_idx; ++i) {
            CBlockIndex* index = cs1.m_chain[i];
            BOOST_CHECK(!index->IsAssumedValid());
            BOOST_CHECK_EQUAL(index->nTx, 0);
        }
    }
}

//! Ensure that snapshot chainstates initialize properly when found on disk.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_init, SnapshotTestSetup)
{
    this->SetupSnapshot();

    ChainstateManager& chainman = *Assert(m_node.chainman);

    fs::path snapshot_chainstate_dir = *node::FindSnapshotChainstateDir();
    BOOST_CHECK(fs::exists(snapshot_chainstate_dir));
    BOOST_CHECK_EQUAL(snapshot_chainstate_dir, gArgs.GetDataDirNet() / "chainstate_snapshot");

    BOOST_CHECK(chainman.IsSnapshotActive());
    const uint256 snapshot_tip_hash = WITH_LOCK(chainman.GetMutex(),
        return chainman.ActiveTip()->GetBlockHash());

    auto all_chainstates = chainman.GetAll();
    BOOST_CHECK_EQUAL(all_chainstates.size(), 2);

    // Test that simulating a shutdown (resetting ChainstateManager) and then performing
    // chainstate reinitializing successfully cleans up the background-validation
    // chainstate data, and we end up with a single chainstate that is at tip.
    ChainstateManager& chainman_restarted = this->SimulateNodeRestart();

    BOOST_TEST_MESSAGE("Performing Load/Verify/Activate of chainstate");

    // This call reinitializes the chainstates.
    this->LoadVerifyActivateChainstate();

    // Restore the txindex normally owned by TestChainSetup so subsequent block
    // generation and fixture teardown use the restarted chainstate.
    g_txindex = std::make_unique<TxIndex>(1 << 20, /*memory=*/true);
    BOOST_REQUIRE(g_txindex->Start(chainman_restarted.ActiveChainstate()));
    IndexWaitSynced(*g_txindex);

    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.GetAll().size(), 2);
        BOOST_CHECK(chainman_restarted.IsSnapshotActive());
        BOOST_CHECK(!chainman_restarted.IsSnapshotValidated());

        BOOST_CHECK_EQUAL(chainman_restarted.ActiveTip()->GetBlockHash(), snapshot_tip_hash);
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 210);
    }

    BOOST_TEST_MESSAGE(
        "Ensure we can mine blocks on top of the initialized snapshot chainstate");
    mineBlocks(10);
    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 220);

        // Background chainstate should be unaware of new blocks on the snapshot
        // chainstate.
        for (Chainstate* cs : chainman_restarted.GetAll()) {
            if (cs != &chainman_restarted.ActiveChainstate()) {
                BOOST_CHECK_EQUAL(cs->m_chain.Height(), 110);
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_init_missing_evodb_marker, SnapshotTestSetup)
{
    this->SetupSnapshot();

    {
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT);
        m_node.evodb->Erase(std::make_pair(EVODB_BEST_BLOCK, uint8_t{1}));
        tx->Commit();
    }
    BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT));

    ChainstateManager& restarted = this->SimulateNodeRestart();
    WITH_LOCK(::cs_main, restarted.InitializeChainstate(
        m_node.mempool.get(), *m_node.evodb, m_node.chain_helper));

    bilingual_str error;
    BOOST_CHECK(!WITH_LOCK(::cs_main, return restarted.DetectSnapshotChainstate(m_node.mempool.get(), error)));
    BOOST_CHECK(error.original.find("Snapshot chainstate EvoDB marker") != std::string::npos);

    WITH_LOCK(::cs_main, restarted.ResetChainstates());
    fs::remove_all(gArgs.GetDataDirNet() / "chainstate_snapshot");
    this->LoadVerifyActivateChainstate();
    g_txindex = std::make_unique<TxIndex>(1 << 20, /*memory=*/true);
    BOOST_REQUIRE(g_txindex->Start(restarted.ActiveChainstate()));
    IndexWaitSynced(*g_txindex);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_init_reindex_missing_evodb_marker, SnapshotReindexTestSetup)
{
    CheckSnapshotReindexLoad(/*reindex=*/true, /*reindex_chainstate=*/false);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_init_reindex_chainstate_missing_evodb_marker, SnapshotReindexTestSetup)
{
    CheckSnapshotReindexLoad(/*reindex=*/false, /*reindex_chainstate=*/true);
}

BOOST_AUTO_TEST_SUITE_END()
