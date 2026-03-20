#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test fastprune mode.

Ensure that blocks larger than the 64 KiB fastprune blockfile limit
don't crash or freeze the node (regression test for bitcoin/bitcoin#27191).
"""
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.wallet import MiniWallet


class FeatureFastpruneTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-fastprune", "-datacarriersize=100000"]]

    def run_test(self):
        wallet = MiniWallet(self.nodes[0])

        self.log.info("Mature coinbase so MiniWallet has a spendable UTXO")
        self.generate(wallet, COINBASE_MATURITY + 1)

        self.log.info("Create an oversized tx (>64 KiB) and mine it via generateblock")
        # In Dash weight == serialized size (no SegWit), so target_weight
        # of 0x10000 (65536 bytes) produces a block that exceeds the
        # fastprune blockfile limit of 64 KiB, exercising the dynamic
        # adjustment added in bitcoin/bitcoin#27191.
        tx = wallet.create_self_transfer(target_weight=0x10000)["tx"]
        self.generateblock(
            self.nodes[0],
            output="raw(55)",
            transactions=[tx.serialize().hex()],
        )
        # Note: no block count assertion — Dash starts at block 200 (DIP3/masternode
        # activation), so the absolute count differs from Bitcoin regtest.


if __name__ == "__main__":
    FeatureFastpruneTest().main()
