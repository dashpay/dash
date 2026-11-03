// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/coinstatsindex.h>
#include <interfaces/chain.h>
#include <node/blockstorage.h>
#include <script/script.h>
#include <test/util/index.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

// Tests of generic BaseIndex functionality that is independent of which
// concrete index is being used. CoinStatsIndex is used here merely as a
// convenient instantiation of BaseIndex.
BOOST_AUTO_TEST_SUITE(baseindex_tests)

// Test that the index does not commit ahead of the chainstate's last
// flushed block. If it did, a subsequent unclean shutdown would corrupt
// the index, because during reverting it would require blocks that were
// never flushed to disk.
BOOST_FIXTURE_TEST_CASE(baseindex_no_commit_ahead_of_flush, TestChain100Setup)
{
    Chainstate& chainstate = Assert(m_node.chainman)->ActiveChainstate();
    auto sync_index = [&](bool do_flush, int expected_sync_height, int expected_commit_height) {
        {
            CoinStatsIndex index{interfaces::MakeChain(m_node), /*n_cache_size=*/1 << 20};
            BOOST_REQUIRE(index.Start());
            IndexWaitSynced(index);
            if (do_flush) {
                chainstate.ForceFlushStateToDisk();
                SyncWithValidationInterfaceQueue();
            }
            BOOST_CHECK_EQUAL(index.GetSummary().best_block_height, expected_sync_height);
            index.Stop();
        }
        // Reload index to see which block data was actually committed. Hold back the
        // sync thread so the loaded best block is not advanced before it is read.
        node::g_indexes_ready_to_sync = false;
        {
            CoinStatsIndex index{interfaces::MakeChain(m_node), /*n_cache_size=*/1 << 20};
            BOOST_REQUIRE(index.Start());
            BOOST_CHECK_EQUAL(index.GetSummary().best_block_height, expected_commit_height);
            index.Interrupt();
            index.Stop();
        }
        node::g_indexes_ready_to_sync = true;
    };

    // Part 1: Sync, then "crash" (stop without flushing). Models a node that
    // started up, had its index catch up, but never flushed before going down.
    // The end-of-sync Commit() runs at chain tip (height 100) but the last
    // flushed block is behind it, so it is skipped.
    sync_index(false, 100, 0);

    // Part 2: Restart cleanly. Sync, force a chainstate flush, and drain the
    // validation queue so the index's ChainStateFlushed callback runs.
    // Now the last flushed block == tip == 100 and the index can commit.
    sync_index(true, 100, 100);

    // Part 3: Connect a new block on the chain without flushing
    // (the last flushed block stays at 100). For a real node this would happen
    // in parallel with Sync(). Here we do it before Sync() to make the race
    // state deterministic.
    CreateAndProcessBlock({}, CScript() << OP_TRUE);
    sync_index(false, 101, 100);
}

BOOST_AUTO_TEST_SUITE_END()
