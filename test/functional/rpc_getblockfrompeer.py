#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the getblockfrompeer RPC."""

from test_framework.authproxy import JSONRPCException
from test_framework.governance import EXPECTED_STDERR_NO_GOV_PRUNE
from test_framework.messages import (
    CBlock,
    from_hex,
    msg_headers,
)
from test_framework.p2p import (
    P2PInterface,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

# Credit-pool reconstruction keeps history from V20 activation until its first snapshot, so postpone V20
# to let this test prune its early blocks.
DEPLOYMENT_ARGS = ["-testactivationheight=v20@3000", "-testactivationheight=mn_rr@3000"]


class GetBlockFromPeerTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.extra_args = [
            DEPLOYMENT_ARGS,
            DEPLOYMENT_ARGS,
            ["-fastprune", "-prune=1"] + DEPLOYMENT_ARGS,
        ]

    def setup_network(self):
        self.setup_nodes()

    def check_for_block(self, node, hash):
        try:
            self.nodes[node].getblock(hash)
            return True
        except JSONRPCException:
            return False

    def run_test(self):
        self.log.info("Mine 4 blocks on Node 0")
        self.generate(self.nodes[0], 4, sync_fun=self.no_op)
        assert_equal(self.nodes[0].getblockcount(), 204)

        self.log.info("Mine competing 3 blocks on Node 1")
        self.generate(self.nodes[1], 3, sync_fun=self.no_op)
        assert_equal(self.nodes[1].getblockcount(), 203)
        short_tip = self.nodes[1].getbestblockhash()

        self.log.info("Connect nodes to sync headers")
        self.connect_nodes(0, 1)
        self.sync_blocks(self.nodes[0:2])

        self.log.info("Node 0 should only have the header for node 1's block 3")
        x = next(filter(lambda x: x['hash'] == short_tip, self.nodes[0].getchaintips()))
        assert_equal(x['status'], "headers-only")
        assert_raises_rpc_error(-1, "Block not found on disk", self.nodes[0].getblock, short_tip)

        self.log.info("Fetch block from node 1")
        peers = self.nodes[0].getpeerinfo()
        assert_equal(len(peers), 1)
        peer_0_peer_1_id = peers[0]["id"]

        self.log.info("Arguments must be valid")
        assert_raises_rpc_error(-8, "hash must be of length 64 (not 4, for '1234')", self.nodes[0].getblockfrompeer, "1234", peer_0_peer_1_id)
        assert_raises_rpc_error(-3, "JSON value of type number is not of expected type string", self.nodes[0].getblockfrompeer, 1234, peer_0_peer_1_id)
        assert_raises_rpc_error(-3, "JSON value of type string is not of expected type number", self.nodes[0].getblockfrompeer, short_tip, "0")

        self.log.info("We must already have the header")
        assert_raises_rpc_error(-1, "Block header missing", self.nodes[0].getblockfrompeer, "00" * 32, 0)

        self.log.info("Non-existent peer generates error")
        for peer_id in [-1, peer_0_peer_1_id + 1]:
            assert_raises_rpc_error(-1, "Peer does not exist", self.nodes[0].getblockfrompeer, short_tip, peer_id)

        self.log.info("Successful fetch")
        result = self.nodes[0].getblockfrompeer(short_tip, peer_0_peer_1_id)
        self.wait_until(lambda: self.check_for_block(node=0, hash=short_tip), timeout=1)
        assert_equal(result, {})

        self.log.info("Don't fetch blocks we already have")
        assert_raises_rpc_error(-1, "Block already downloaded", self.nodes[0].getblockfrompeer, short_tip, peer_0_peer_1_id)

        self.log.info("Don't fetch blocks while the node has not synced past it yet")
        # For this test we need node 1 in prune mode and as a side effect this also disconnects
        # the nodes which is also necessary for the rest of the test.
        self.restart_node(1, ["-prune=550"] + DEPLOYMENT_ARGS)

        # Generate a block on the disconnected node that the pruning node is not connected to
        blockhash = self.generate(self.nodes[0], 1, sync_fun=self.no_op)[0]
        block_hex = self.nodes[0].getblock(blockhash=blockhash, verbosity=0)
        block = from_hex(CBlock(), block_hex)

        # Connect a P2PInterface to the pruning node and have it submit only the header of the
        # block that the pruning node has not seen
        node1_interface = self.nodes[1].add_p2p_connection(P2PInterface())
        node1_interface.send_and_ping(msg_headers([block]))

        # Get the peer id of the P2PInterface from the pruning node
        node1_peers = self.nodes[1].getpeerinfo()
        assert_equal(len(node1_peers), 1)
        node1_interface_id = node1_peers[0]["id"]

        # Trying to fetch this block from the P2PInterface should not be possible
        error_msg = "In prune mode, only blocks that the node has already synced previously can be fetched from a peer"
        assert_raises_rpc_error(-1, error_msg, self.nodes[1].getblockfrompeer, blockhash, node1_interface_id)
        self.stop_node(1, expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)

        self.log.info("Connect pruned node")
        # We need to generate more blocks to be able to prune
        self.connect_nodes(0, 2)
        pruned_node = self.nodes[2]
        # Dash blocks are smaller than Bitcoin's, so many more of them fit into a
        # (-fastprune) block file. More blocks are needed before the first block
        # file wraps and can be pruned, and the resulting prune heights are not the
        # same as upstream, hence they are not compared against fixed values here.
        self.generate(self.nodes[0], 900, sync_fun=self.no_op)
        self.sync_blocks([self.nodes[0], pruned_node])
        pruneheight = pruned_node.pruneblockchain(800)
        assert_greater_than(pruneheight, 2)
        # Ensure the block is actually pruned
        pruned_block = self.nodes[0].getblockhash(2)
        assert_raises_rpc_error(-1, "Block not available (pruned data)", pruned_node.getblock, pruned_block)

        self.log.info("Fetch pruned block")
        peers = pruned_node.getpeerinfo()
        assert_equal(len(peers), 1)
        pruned_node_peer_0_id = peers[0]["id"]
        result = pruned_node.getblockfrompeer(pruned_block, pruned_node_peer_0_id)
        self.wait_until(lambda: self.check_for_block(node=2, hash=pruned_block), timeout=1)
        assert_equal(result, {})

        self.log.info("Fetched block persists after next pruning event")
        tip_at_fetch = pruned_node.getblockcount()
        self.generate(self.nodes[0], 250, sync_fun=self.no_op)
        self.sync_blocks([self.nodes[0], pruned_node])
        # Prune below the height the tip had when the block was fetched: the block
        # file the re-fetched block was written to still holds recent blocks and is
        # therefore kept.
        prev_pruneheight = pruneheight
        pruneheight = pruned_node.pruneblockchain(tip_at_fetch - 55)
        assert_greater_than(pruneheight, prev_pruneheight)
        assert_equal(pruned_node.getblock(pruned_block)["hash"], pruned_block)

        self.log.info("Fetched block can be pruned again when prune height exceeds the height of the tip at the time when the block was fetched")
        self.generate(self.nodes[0], 1200, sync_fun=self.no_op)
        self.sync_blocks([self.nodes[0], pruned_node])
        assert_greater_than(pruned_node.pruneblockchain(tip_at_fetch + 1000), tip_at_fetch)
        assert_raises_rpc_error(-1, "Block not available (pruned data)", pruned_node.getblock, pruned_block)
        self.stop_node(2, expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)


if __name__ == '__main__':
    GetBlockFromPeerTest().main()
