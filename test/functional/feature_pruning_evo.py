#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that pruning keeps the blocks evo validation reads back from disk.

After V20, connecting a block on a cold credit pool cache (e.g. after a restart)
rebuilds the credit pool from the last disk snapshot, taken every 576 blocks. It
reads every block on the way and the block CreditPoolPeriodBlocks before each of
them, which is further back than MIN_BLOCKS_TO_KEEP. A pruned node has to keep
these blocks, also when pruning manually and for the blocks it connects after a
reorg, or it rejects valid blocks. It refuses to start on a datadir pruned
without them.
"""

import os

from test_framework.governance import EXPECTED_STDERR_NO_GOV_PRUNE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_greater_than_or_equal

V20_HEIGHT = 150
DEPLOYMENT_ARGS = [
    "-dip3params=2:2",
    f"-testactivationheight=v20@{V20_HEIGHT}",
    f"-testactivationheight=mn_rr@{V20_HEIGHT}",
]
MIN_BLOCKS_TO_KEEP = 288
CREDIT_POOL_SNAPSHOT_PERIOD = 576  # CCreditPoolManager::DISK_SNAPSHOT_PERIOD
CREDIT_POOL_PERIOD_BLOCKS = 100  # CChainParams::CreditPoolPeriodBlocks() on regtest
# Validating the next blocks reads this many blocks below the tip, the evo prune lock keeps MIN_BLOCKS_TO_KEEP more
# for a reorg
EVO_BLOCKS_READ = CREDIT_POOL_SNAPSHOT_PERIOD + CREDIT_POOL_PERIOD_BLOCKS
EVO_BLOCKS_TO_KEEP = MIN_BLOCKS_TO_KEEP + EVO_BLOCKS_READ
PRUNE_LOCK_BUFFER = 10


class FeaturePruningEvoTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            DEPLOYMENT_ARGS,
            ["-prune=1", "-fastprune", "-debug=prune"] + DEPLOYMENT_ARGS,
        ]

    def mine_to(self, height):
        while self.nodes[0].getblockcount() < height:
            self.generate(self.nodes[0], min(250, height - self.nodes[0].getblockcount()))

    def prune_and_restart(self, tip):
        """Mine to tip, prune as far as allowed and restart the pruned node, so that its credit pool cache is cold"""
        self.mine_to(tip)
        pruned_height = self.nodes[1].pruneblockchain(tip)
        assert_greater_than(pruned_height, self.prev_pruned_height)
        self.prev_pruned_height = pruned_height
        self.restart_node(1, extra_args=self.extra_args[1], expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)
        return pruned_height

    def check_evo_prune_lock(self, tip):
        lock_height = tip - EVO_BLOCKS_TO_KEEP - PRUNE_LOCK_BUFFER - 1
        with self.nodes[1].assert_debug_log([f"evo limited pruning to height {lock_height}"]):
            self.nodes[1].pruneblockchain(tip)

    def run_test(self):
        miner, pruned = self.nodes
        self.prev_pruned_height = 0

        self.log.info("Reorg a pruned node by MIN_BLOCKS_TO_KEEP blocks after a restart at the fork point")
        # The first block connected after the reorg rebuilds the credit pool from the snapshot before the fork point,
        # which reaches furthest back when the fork point is one block before a snapshot height
        fork_height = 2 * CREDIT_POOL_SNAPSHOT_PERIOD - 1
        tip = fork_height + MIN_BLOCKS_TO_KEEP
        pruned_height = self.prune_and_restart(tip)
        fork_child = pruned.getblockhash(fork_height + 1)
        pruned.invalidateblock(fork_child)
        assert_equal(pruned.getblockcount(), fork_height)
        # Restart at the fork point, which reconsiders the invalid blocks ahead of the tip
        fork_hash = pruned.getblockhash(fork_height)
        with pruned.assert_debug_log([f"Loaded best chain: hashBestChain={fork_hash} height={fork_height}",
                                      "is ahead of our tip, reconsidering"]):
            self.restart_node(1, extra_args=self.extra_args[1], expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)
        pruned.reconsiderblock(fork_child)
        assert_equal(pruned.getbestblockhash(), miner.getbestblockhash())
        last_snapshot = fork_height - fork_height % CREDIT_POOL_SNAPSHOT_PERIOD
        assert_greater_than(last_snapshot + 1 - CREDIT_POOL_PERIOD_BLOCKS, pruned_height)
        # It started at the fork point although the blocks that the lock would keep there for a further reorg, below
        # the ones that validating the next blocks reads, were pruned
        assert_greater_than(pruned_height, fork_height - EVO_BLOCKS_TO_KEEP)
        self.check_evo_prune_lock(tip)
        self.connect_nodes(0, 1)

        self.log.info("Restart a pruned node at a credit pool snapshot height and connect the next block")
        # The snapshot of the tip is only written once the next block connects, so the rebuild starts from the
        # snapshot before it
        tip = 3 * CREDIT_POOL_SNAPSHOT_PERIOD
        pruned_height = self.prune_and_restart(tip)
        self.connect_nodes(0, 1)
        self.generate(miner, 1)
        last_snapshot = tip - CREDIT_POOL_SNAPSHOT_PERIOD
        assert_greater_than(last_snapshot + 1 - CREDIT_POOL_PERIOD_BLOCKS, pruned_height)
        self.check_evo_prune_lock(tip + 1)

        self.log.info("Refuse to start on a datadir missing blocks that validating the next blocks reads")
        # Without the lock, pruneblockchain prunes up to MIN_BLOCKS_TO_KEEP blocks below the tip. Prune blocks that
        # validating the next blocks reads, further back than the ones building the credit pool reads.
        self.restart_node(1, extra_args=self.extra_args[1] + ["-evoprunelock=0"], expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)
        last_snapshot = tip
        tip = pruned.getblockcount()
        pruned_height = pruned.pruneblockchain(tip)
        assert_greater_than_or_equal(pruned_height, tip - EVO_BLOCKS_READ)
        assert_greater_than(last_snapshot + 1 - CREDIT_POOL_PERIOD_BLOCKS, pruned_height)
        self.stop_node(1, expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)
        # The error names the newest missing block, the last pruned one
        pruned.assert_start_raises_init_error(
            extra_args=self.extra_args[1],
            expected_msg=f"{EXPECTED_STDERR_NO_GOV_PRUNE}{os.linesep}: Block {pruned_height}, which is needed to "
                         "validate new blocks, has been pruned. You need to rebuild the database using -reindex. This "
                         f"will redownload the entire blockchain.{os.linesep}"
                         "Please restart with -reindex to recover.",
        )

        self.log.info("Recover with -reindex")
        self.start_node(1, extra_args=self.extra_args[1] + ["-reindex"])
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(pruned.getbestblockhash(), miner.getbestblockhash())

        self.stop_node(1, expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)


if __name__ == '__main__':
    FeaturePruningEvoTest().main()
