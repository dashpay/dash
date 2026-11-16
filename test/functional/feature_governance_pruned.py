#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test governance on a pruned node.

A pruned node can't run -txindex, so it looks proposal collateral up in the governance collateral index.
"""

import os
import shutil

from test_framework.governance import EXPECTED_STDERR_GOV_PRUNED_TOO_FAR, prepare_object
from test_framework.messages import uint256_to_string
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, force_finish_mnsync

MIN_BLOCKS_TO_KEEP = 288 # src/validation.h


class GovernancePrunedNodeTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_dash_test_params(2, 0, [[], ["-prune=550", "-fastprune"]])

    def run_test(self):
        node = self.nodes[0]
        pruned_node = self.nodes[1]
        assert pruned_node.getblockchaininfo()["pruned"]
        assert "txindex" not in pruned_node.getindexinfo()

        # Without InstantSend locks, the miner leaves out the transactions it would otherwise wait on
        node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        self.log.info("Prepare a proposal on the pruned node")
        node.sendtoaddress(pruned_node.getnewaddress(), 10)
        self.bump_mocktime(1)
        self.generate(node, 1)
        self.wait_until(lambda: pruned_node.getbalance() == 10)
        proposal_time = self.mocktime
        proposal = prepare_object(pruned_node, 1, uint256_to_string(0), proposal_time, 1, "Proposal", 1, pruned_node.getnewaddress())
        self.sync_mempools()
        self.bump_mocktime(6)
        self.generate(node, 6)

        def submit():
            return pruned_node.gobject("submit", "0", 1, proposal["createdAt"], proposal["hex"], proposal["collateralHash"])

        self.log.info("The pruned node finds collateral whose block it has pruned")
        collateral_height = node.getrawtransaction(proposal["collateralHash"], True)["height"]
        # Each -fastprune block file holds dozens of blocks, so mine enough to prune the collateral's whole file
        self.bump_mocktime(MIN_BLOCKS_TO_KEEP + 150)
        self.generate(node, MIN_BLOCKS_TO_KEEP + 150)
        pruned_node.pruneblockchain(pruned_node.getblockcount() - MIN_BLOCKS_TO_KEEP)
        assert pruned_node.getblockchaininfo()["pruneheight"] > collateral_height
        assert_raises_rpc_error(-1, "Block not available (pruned data)", pruned_node.getblock,
                                pruned_node.getblockhash(collateral_height))
        proposal_hash = submit()
        self.wait_until(lambda: proposal_hash in node.gobject("list", "valid", "proposals"))

        self.log.info("Governance objects survive a restart of the pruned node")
        self.restart_node(1)
        force_finish_mnsync(pruned_node)
        self.connect_nodes(0, 1)
        assert proposal_hash in pruned_node.gobject("list", "valid", "proposals")
        self.log.info("The reopened index still finds the pruned collateral")
        self.wait_until(lambda: pruned_node.getindexinfo()["govcollateralindex"]["synced"])
        assert_equal(submit(), proposal_hash)

        self.log.info("A node that pruned before it had the index starts with governance off instead of failing")
        self.stop_node(1)
        shutil.rmtree(os.path.join(pruned_node.chain_path, "indexes", "govcollateralindex"))
        self.start_node(1)
        assert "govcollateralindex" not in pruned_node.getindexinfo()
        assert_raises_rpc_error(-1, "Preparing a proposal now would burn its fee", pruned_node.gobject, "prepare",
                                "0", 1, proposal["createdAt"], proposal["hex"])
        assert_raises_rpc_error(-1, "Governance is disabled on this node", submit)
        self.stop_node(1, expected_stderr=EXPECTED_STDERR_GOV_PRUNED_TOO_FAR)

        self.log.info("A -reindex downloads the chain again, builds the index and turns governance back on")
        self.start_node(1, extra_args=self.extra_args[1] + ["-reindex"])
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert pruned_node.getblockchaininfo()["pruned"]
        self.wait_until(lambda: pruned_node.getindexinfo()["govcollateralindex"]["synced"])
        self.bump_mocktime(5 * 60)

        def governance_synced():
            self.bump_mocktime(1)
            return pruned_node.mnsync("status")["IsSynced"]
        self.wait_until(governance_synced)
        self.wait_until(lambda: proposal_hash in pruned_node.gobject("list", "valid", "proposals"))

        self.log.info("The governance collateral index runs and finds collateral during -reindex-chainstate")
        height = node.getblockcount()
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex-chainstate"])
        self.wait_until(lambda: node.getblockcount() == height)
        self.wait_until(lambda: node.getindexinfo()["govcollateralindex"]["synced"])
        force_finish_mnsync(node)
        assert_equal(node.gobject("submit", "0", 1, proposal["createdAt"], proposal["hex"], proposal["collateralHash"]),
                     proposal_hash)


if __name__ == '__main__':
    GovernancePrunedNodeTest().main()
