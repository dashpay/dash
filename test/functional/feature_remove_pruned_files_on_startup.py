#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test removing undeleted pruned blk files on startup."""

import platform
import os
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet

class FeatureRemovePrunedFilesOnStartupTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-fastprune", "-prune=1"]]
        if self.is_zstd_compiled():
            self.extra_args[0].append("-blockcompression=3")

    def mine_batches(self, blocks):
        n = blocks // 250
        for _ in range(n):
            self.generate(self.wallet, 250)
        self.generate(self.wallet, blocks % 250)
        self.sync_blocks()

    def has_compressed_undo_record(self, rev_file):
        data = rev_file.read_bytes()
        pos = 0
        while pos + 8 <= len(data) and data[pos:pos + 4] != bytes(4):
            size_field = int.from_bytes(data[pos + 4:pos + 8], "little")
            if size_field & 0x80000000:
                return True
            # each undo record is followed by its 32-byte checksum
            pos += 8 + (size_field & 0x7fffffff) + 32
        return False

    def run_test(self):
        blk0 = self.nodes[0].blocks_path / "blk00000.dat"
        rev0 = self.nodes[0].blocks_path / "rev00000.dat"
        blk1 = self.nodes[0].blocks_path / "blk00001.dat"
        rev1 = self.nodes[0].blocks_path / "rev00001.dat"
        self.wallet = MiniWallet(self.nodes[0])
        self.mine_batches(1200)
        # a block spending many coins with the same script, so that its undo data compresses
        self.wallet.send_self_transfer_multi(from_node=self.nodes[0], utxos_to_spend=self.wallet.get_utxos()[:50], fee_per_output=10000)
        spend_block = self.generate(self.wallet, 1)[0]
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
            newest_blk = sorted(self.nodes[0].blocks_path.glob("blk*.dat"))[-1]
            with open(newest_blk, "rb") as f:
                size_field = int.from_bytes(f.read(8)[4:], "little")
            assert size_field & 0x80000000, f"{newest_blk.name} first record is not compressed"
            newest_rev = sorted(self.nodes[0].blocks_path.glob("rev*.dat"))[-1]
            assert self.has_compressed_undo_record(newest_rev), f"{newest_rev.name} holds no compressed undo record"
            self.log.info("Compressed undo data is still readable")
            assert_equal(self.nodes[0].getblockstats(spend_block)["ins"], 50)
        assert_raises_rpc_error(-1, "Block not available (pruned data)", self.nodes[0].getblock, self.nodes[0].getblockhash(100))

        # Check that the files are removed on restart once the fds are closed
        fd1.close()
        fd2.close()
        self.restart_node(0)
        assert not os.path.exists(blk0)
        assert not os.path.exists(rev1)
        self.stop_node(0)

if __name__ == '__main__':
    FeatureRemovePrunedFilesOnStartupTest().main()
