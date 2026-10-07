#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test removing undeleted pruned blk files on startup."""

import platform
import os
from test_framework.governance import EXPECTED_STDERR_NO_GOV_PRUNE
from test_framework.test_framework import BitcoinTestFramework

# Credit-pool reconstruction keeps history from V20 activation until its first snapshot, so postpone V20
# to let this test prune its early blocks.
DEPLOYMENT_ARGS = ["-testactivationheight=v20@2000", "-testactivationheight=mn_rr@2000"]


class FeatureRemovePrunedFilesOnStartupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-fastprune", "-prune=1"] + DEPLOYMENT_ARGS]

    def mine_batches(self, blocks):
        n = blocks // 250
        for _ in range(n):
            self.generate(self.nodes[0], 250)
        self.generate(self.nodes[0], blocks % 250)
        self.sync_blocks()

    def run_test(self):
        blk0 = os.path.join(self.nodes[0].datadir, self.nodes[0].chain, 'blocks', 'blk00000.dat')
        rev0 = os.path.join(self.nodes[0].datadir, self.nodes[0].chain, 'blocks', 'rev00000.dat')
        blk1 = os.path.join(self.nodes[0].datadir, self.nodes[0].chain, 'blocks', 'blk00001.dat')
        rev1 = os.path.join(self.nodes[0].datadir, self.nodes[0].chain, 'blocks', 'rev00001.dat')
        self.mine_batches(800)
        fo1 = os.open(blk0, os.O_RDONLY)
        fo2 = os.open(rev1, os.O_RDONLY)
        fd1 = os.fdopen(fo1)
        fd2 = os.fdopen(fo2)
        self.nodes[0].pruneblockchain(600)

        # Windows systems will not remove files with an open fd
        if platform.system() != 'Windows':
            assert not os.path.exists(blk0)
            assert not os.path.exists(rev0)
            assert not os.path.exists(blk1)
            assert not os.path.exists(rev1)
        else:
            assert os.path.exists(blk0)
            assert not os.path.exists(rev0)
            assert not os.path.exists(blk1)
            assert os.path.exists(rev1)

        # Check that the files are removed on restart once the fds are closed
        fd1.close()
        fd2.close()
        self.restart_node(0, expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)
        assert not os.path.exists(blk0)
        assert not os.path.exists(rev1)
        self.stop_node(0, expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)

if __name__ == '__main__':
    FeatureRemovePrunedFilesOnStartupTest().main()
