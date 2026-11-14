#!/usr/bin/env python3
# Copyright (c) 2014-2020 The Bitcoin Core developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a getblocktemplate longpoll returns once the mempool changes.

The RPC only looks at the mempool after a minute on the steady clock, which
mocktime can't shorten, so this takes about 70 seconds.
"""

import random

from mining_getblocktemplate_longpoll import LongpollThread
from test_framework.test_framework import BitcoinTestFramework
from test_framework.wallet import MiniWallet


class GetBlockTemplateLPMempoolTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.supports_cli = False

    def run_test(self):
        self.generate(self.nodes[0], 10)
        miniwallet = MiniWallet(self.nodes[0])

        self.log.info("Test that introducing a new transaction into the mempool will terminate the longpoll")
        thr = LongpollThread(self.nodes[0])
        with self.nodes[0].assert_debug_log(["Received a POST request for / from"], timeout=3):
            thr.start()
        miniwallet.send_self_transfer(from_node=random.choice(self.nodes))
        # after one minute, every 10 seconds the mempool is probed, so in 80 seconds it should have returned

        def check():
            self.bump_mocktime(1)
            return not thr.is_alive()
        self.wait_until(check, timeout=60 + 20)


if __name__ == '__main__':
    GetBlockTemplateLPMempoolTest().main()
