#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test removing undeleted pruned blk files on startup."""

import platform
import os
from test_framework.governance import EXPECTED_STDERR_NO_GOV_PRUNE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

class FeatureRemovePrunedFilesOnStartupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-fastprune", "-prune=1", "-blockcompression=3"]]

    def mine_batches(self, blocks):
        n = blocks // 250
        for _ in range(n):
            self.generate(self.nodes[0], 250)
        self.generate(self.nodes[0], blocks % 250)
        self.sync_blocks()

    def run_test(self):
        blk0 = self.nodes[0].blocks_path / "blk00000.dat"
        rev0 = self.nodes[0].blocks_path / "rev00000.dat"
        blk1 = self.nodes[0].blocks_path / "blk00001.dat"
        rev1 = self.nodes[0].blocks_path / "rev00001.dat"
        self.mine_batches(1200)
        fo1 = os.open(blk0, os.O_RDONLY)
        fo2 = os.open(rev1, os.O_RDONLY)
        fd1 = os.fdopen(fo1)
        fd2 = os.fdopen(fo2)
        self.nodes[0].pruneblockchain(1000)

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

        self.log.info("Blocks left on disk are still readable")
        assert_equal(self.nodes[0].getblock(self.nodes[0].getblockhash(1100))["height"], 1100)
        if self.is_zstd_compiled():
            self.log.info("Blocks left on disk are stored zstd-compressed")
            for blk in sorted(self.nodes[0].blocks_path.glob("blk*.dat")):
                with open(blk, "rb") as f:
                    size_field = int.from_bytes(f.read(8)[4:], "little")
                assert size_field & 0x80000000, f"{blk.name} first record is not compressed"
        assert_raises_rpc_error(-1, "Block not available (pruned data)", self.nodes[0].getblock, self.nodes[0].getblockhash(100))

        # Check that the files are removed on restart once the fds are closed
        fd1.close()
        fd2.close()
        self.restart_node(0, expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)
        assert not os.path.exists(blk0)
        assert not os.path.exists(rev1)
        self.stop_node(0, expected_stderr=EXPECTED_STDERR_NO_GOV_PRUNE)

if __name__ == '__main__':
    FeatureRemovePrunedFilesOnStartupTest().main()
