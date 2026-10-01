#!/usr/bin/env python3
# Copyright (c) 2015-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_is_retroactive_session_timeout.py

Tests retroactive signing of a partially known TX once the signing session for
its IS lock timed out, with and without cycling the LLMQs in between.

We have 5 nodes where node 0 is the control node, nodes 1-4 are masternodes.
Mempool inconsistencies are simulated via disconnecting/reconnecting node 3
and by having a higher relay fee on node 4.
'''

import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import set_node_times


class LLMQ_IS_RetroactiveSigningSessionTimeout(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        # -whitelist is needed to avoid the trickling logic on node0
        self.set_dash_test_params(5, 4, [["-whitelist=127.0.0.1"], [], [], [], ["-minrelaytxfee=0.001"]])

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def assert_no_instantlock(self, txid, node):
        self.log.info(f"Expecting no InstantLock for {txid}")
        assert not node.getrawtransaction(txid, True)["instantlock"]

    def wait_for_log_line(self, nodes, fragment):
        def logged(node):
            with open(node.debug_log_path, "rb") as f:
                return fragment.encode() in f.read()
        for node in nodes:
            self.wait_until(lambda: logged(node))

    def wait_for_sigshares(self, nodes, txid):
        # Members sign asynchronously and the sig share log line is the only signal that they are done
        self.wait_for_log_line(nodes, f"created sigShare. signHash=")
        self.wait_for_log_line(nodes, f"msgHash={txid}, llmqType=")

    def create_fund_sign_tx(self):
        rawtx = self.nodes[0].createrawtransaction([], {self.nodes[0].getnewaddress(): 1})
        rawtx = self.nodes[0].fundrawtransaction(rawtx)['hex']
        rawtx = self.nodes[0].signrawtransactionwithwallet(rawtx)['hex']
        return rawtx

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        # Mempool IS signing on
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 0)
        self.wait_for_sporks_same()

        self.mine_cycle_quorum()

        # Make sure that all nodes are chainlocked at the same height before starting actual tests
        self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash(), timeout=30)

        self.log.info("testing retroactive signing with partially known TX and session timeout")
        self.test_session_timeout(False)
        self.log.info("repeating test, but with cycled LLMQs")
        self.test_session_timeout(True)

    def test_session_timeout(self, do_cycle_llmqs):
        set_node_times(self.nodes, self.mocktime)
        self.isolate_node(3)
        rawtx = self.create_fund_sign_tx()
        txid_all_nodes = self.nodes[0].sendrawtransaction(rawtx)
        txid_all_nodes = self.nodes[3].sendrawtransaction(rawtx)

        rawtx_1 = self.create_fund_sign_tx()
        txid_single_node = self.nodes[3].sendrawtransaction(rawtx_1)

        # Make sure connected nodes received the TX before we continue
        self.bump_mocktime(30)
        self.sync_mempools(self.nodes[:3])
        # Make sure signing is done on nodes 1 and 2 (it's async)
        self.wait_for_sigshares(self.nodes[1:3], txid_all_nodes)
        self.wait_for_sigshares([self.nodes[3]], txid_single_node)
        # Make the signing session for the IS lock timeout on nodes 1-3
        self.bump_mocktime(61)
        time.sleep(2) # make sure Cleanup() is called
        self.reconnect_isolated_node(3, 0)
        # Make sure nodes actually try re-connecting quorum connections
        self.bump_mocktime(30)
        self.wait_for_mnauth(self.nodes[3], 2)

        self.nodes[0].sendrawtransaction(rawtx_1)
        # Make sure connected nodes received the TX
        self.bump_mocktime(30)
        self.sync_mempools(self.nodes[:3])
        self.bump_mocktime(30)

        # Make sure signing is done on nodes 1 and 2 (it's async)
        self.wait_for_sigshares(self.nodes[1:3], txid_single_node)

        # node 3 fully reconnected but the signing session is already timed out, so no IS lock
        self.assert_no_instantlock(txid_all_nodes, self.nodes[0])
        self.assert_no_instantlock(txid_single_node, self.nodes[0])
        if do_cycle_llmqs:
            self.mine_cycle_quorum()
            self.mine_cycle_quorum()
            self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash(), timeout=30)

            time.sleep(5)
            self.assert_no_instantlock(txid_all_nodes, self.nodes[0])
            self.assert_no_instantlock(txid_single_node, self.nodes[0])
        # Make node 0 consider the TX as safe
        self.bump_mocktime(10 * 60 + 1)
        block = self.generate(self.nodes[0], 1, sync_fun=self.no_op)[0]
        assert txid_all_nodes in self.nodes[0].getblock(block, 1)['tx']
        assert txid_single_node in self.nodes[0].getblock(block, 1)['tx']
        self.wait_for_chainlocked_block_all_nodes(block)

if __name__ == '__main__':
    LLMQ_IS_RetroactiveSigningSessionTimeout().main()
