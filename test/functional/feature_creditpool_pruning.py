#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Retain credit-pool reconstruction history across manual pruning and restart."""

from test_framework.governance import EXPECTED_STDERR_NO_GOV_PRUNE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

# Node 1 starts unpruned: a -prune=1 node started on an empty chain trips an
# unrelated UBSan overflow in the init-time disk space check.
NODE1_ARGS = ["-fastprune", "-txindex=0", "-checkblocks=0", "-debug=prune"]
PRUNE_ARGS = NODE1_ARGS + ["-prune=1", "-disablegovernance"]


class CreditPoolPruningTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [[], NODE1_ARGS]

    def restart_pruned(self):
        expected_stderr = EXPECTED_STDERR_NO_GOV_PRUNE if self.nodes[1].getblockchaininfo()["pruned"] else ""
        self.restart_node(1, extra_args=PRUNE_ARGS, expected_stderr=expected_stderr)
        self.connect_nodes(0, 1)
        self.sync_blocks()

    def run_test(self):
        full, pruned = self.nodes
        self.generate(full, 1100)
        assert_equal(full.getdeploymentinfo()["deployments"]["v20"]["active"], True)
        first_block = full.getblockhash(1)
        # Snapshot 576 plus one, less the regtest 100-block withdrawal window.
        dependency = full.getblockhash(477)

        self.log.info("Prune cold, before the first credit-pool lookup")
        self.restart_pruned()
        # The common lock keeps a ten-block buffer below the first required body.
        with pruned.assert_debug_log(["creditpool limited pruning to height 466"]):
            prune_height = pruned.pruneblockchain(1100)
        assert prune_height < 477
        assert_raises_rpc_error(-1, "Block not available (pruned data)", pruned.getblock, first_block)
        assert_equal(pruned.getblock(dependency)["height"], 477)
        self.generate(full, 1)
        assert_equal(pruned.getcreditpoolinfo(), full.getcreditpoolinfo())

        self.log.info("Keep older dependencies at the next snapshot boundary")
        self.generate(full, 1152 - full.getblockcount())
        assert pruned.pruneblockchain(1152) < 477
        assert_equal(pruned.getblock(dependency)["height"], 477)
        self.generate(full, 1)

        self.log.info("Advance retention once snapshot 1152 covers the supported reorg range")
        self.generate(full, 1440 - full.getblockcount())
        newer_dependency = full.getblockhash(1053)
        self.restart_pruned()
        with pruned.assert_debug_log(["creditpool limited pruning to height 1042"]):
            prune_height = pruned.pruneblockchain(1440)
        assert 477 < prune_height < 1053
        assert_raises_rpc_error(-1, "Block not available (pruned data)", pruned.getblock, dependency)
        assert_equal(pruned.getblock(newer_dependency)["height"], 1053)
        self.generate(full, 1)
        assert_equal(pruned.getcreditpoolinfo(), full.getcreditpoolinfo())

        # Bodies below 1053 are already gone; this only checks the lock follows the lower tip.
        self.log.info("Recompute retention after a short tip rollback")
        disconnected = pruned.getblockhash(1438)
        pruned.invalidateblock(disconnected)
        assert_equal(pruned.getblockcount(), 1437)
        with pruned.assert_debug_log(["creditpool limited pruning to height 466"]):
            assert pruned.pruneblockchain(1437) < 1053
        pruned.reconsiderblock(disconnected)
        self.sync_blocks()
        self.generate(full, 1)
        assert_equal(pruned.getcreditpoolinfo(), full.getcreditpoolinfo())
        self.stop_node(1, expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)


if __name__ == '__main__':
    CreditPoolPruningTest().main()
