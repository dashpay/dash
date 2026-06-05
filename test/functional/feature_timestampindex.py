#!/usr/bin/env python3
# Copyright (c) 2014-2015 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test timestampindex generation and fetching
#

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class TimestampIndexTest(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def setup_network(self):
        self.add_nodes(self.num_nodes)
        self.start_node(0)
        self.start_node(1, ["-timestampindex"])
        self.connect_nodes(0, 1)

    def run_test(self):
        self.log.info("Test that settings can be disabled without -reindex...")
        self.generate(self.nodes[0], 1)
        self.stop_node(1)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        self.start_node(1, ["-timestampindex=0"])
        self.connect_nodes(0, 1)
        self.sync_all()

        self.log.info("Test that settings can be enabled without -reindex...")
        self.stop_node(1)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        self.start_node(1, ["-timestampindex"])
        self.connect_nodes(0, 1)
        self.sync_all()

        self.log.info("Check timestamp index via getblockhashes rpc")
        blockhashes = []
        for _ in range(5):
            self.bump_mocktime(1)
            blockhashes += self.generate(self.nodes[0], 1)
        low = self.nodes[0].getblock(blockhashes[0])["time"]
        high = self.nodes[0].getblock(blockhashes[4])["time"]
        self.bump_mocktime(1)
        hashes = self.nodes[1].getblockhashes(high, low)
        assert_equal(len(hashes), 5)
        assert_equal(sorted(blockhashes), sorted(hashes))

        self.log.info("Testing reorg handling...")
        # Invalidate the last 2 blocks on both nodes
        self.nodes[0].invalidateblock(blockhashes[3])
        self.nodes[1].invalidateblock(blockhashes[3])

        # Verify that invalidated blocks are no longer in the index
        hashes_after_invalidate = self.nodes[1].getblockhashes(high, low)
        assert_equal(len(hashes_after_invalidate), 3)
        assert_equal(sorted(hashes_after_invalidate), sorted(blockhashes[:3]))

        # Reconsider the blocks on both nodes
        self.nodes[0].reconsiderblock(blockhashes[3])
        self.nodes[1].reconsiderblock(blockhashes[3])
        self.sync_all()

        # Verify all blocks are back in the index
        hashes_after_reconsider = self.nodes[1].getblockhashes(high, low)
        assert_equal(len(hashes_after_reconsider), 5)
        assert_equal(sorted(hashes_after_reconsider), sorted(blockhashes))


if __name__ == '__main__':
    TimestampIndexTest().main()
