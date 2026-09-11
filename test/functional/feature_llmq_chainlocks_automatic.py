#!/usr/bin/env python3
# Copyright (c) 2025-2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Recover ChainLocks from blocks without receiving CLSIG messages."""

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, force_finish_mnsync


class LLMQChainLocksAutomaticTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_dash_test_params(2, 1)
        self.set_dash_llmq_test_params(1, 1)
        self.delay_v20_and_mn_rr(height=200)
        self.extra_args[1].append("-sporkkey=cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK")

    def run_test(self):
        self.activate_v20(expected_activation_height=200)
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.nodes[0].sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", 0)
        self.wait_for_sporks_same()
        self.mine_quorum_single_member()
        self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash())

        receiver, producer = self.nodes
        self.log.info("Isolate the regular node and deliver only raw blocks over RPC")
        self.isolate_node(0)
        signed_hash = self.generate(producer, 1, sync_fun=self.no_op)[0]
        self.wait_for_chainlocked_block(producer, signed_hash)
        expected = producer.getbestchainlock()
        assert_equal(expected["blockhash"], signed_hash)
        assert_equal(receiver.submitblock(producer.getblock(signed_hash, 0)), None)
        assert receiver.getbestchainlock()["height"] < expected["height"]

        # Stop signing so every subsequent coinbase repeats the same signature.
        self.bump_mocktime(1)
        producer.sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", 1)
        assert_equal(producer.spork("show")["SPORK_19_CHAINLOCKS_ENABLED"], 1)
        carrier = self.generate(producer, 1, sync_fun=self.no_op)[0]
        cbtx = producer.getblock(carrier, 2)["cbTx"]
        assert_equal(cbtx["bestCLHeightDiff"], 0)
        assert_equal(cbtx["bestCLSignature"], expected["signature"])
        assert_equal(receiver.submitblock(producer.getblock(carrier, 0)), None)
        self.wait_for_chainlocked_block(receiver, signed_hash)
        assert_equal(receiver.getbestchainlock(), expected)
        assert_equal(receiver.getconnectioncount(), 0)

        self.log.info("Recover a nonzero-offset ChainLock after restart")
        self.restart_node(0, extra_args=self.extra_args[0] + ["-connect=0"])
        receiver.setnetworkactive(False)
        force_finish_mnsync(receiver)
        carrier = self.generate(producer, 1, sync_fun=self.no_op)[0]
        cbtx = producer.getblock(carrier, 2)["cbTx"]
        assert_equal(cbtx["bestCLHeightDiff"], 1)
        assert_equal(cbtx["height"] - cbtx["bestCLHeightDiff"] - 1, expected["height"])
        assert_equal(receiver.submitblock(producer.getblock(carrier, 0)), None)
        self.wait_for_chainlocked_block(receiver, signed_hash)
        assert_equal(receiver.getbestchainlock(), expected)
        assert_equal(receiver.getconnectioncount(), 0)

        self.log.info("Repeated coinbase signatures leave the best ChainLock unchanged")
        carrier = self.generate(producer, 1, sync_fun=self.no_op)[0]
        assert_equal(receiver.submitblock(producer.getblock(carrier, 0)), None)
        receiver.syncwithvalidationinterfacequeue()
        assert_equal(receiver.getbestchainlock(), expected)

        self.log.info("Normal ChainLock relay continues after reconnection")
        self.reconnect_isolated_node(0, 1)
        self.bump_mocktime(1)
        producer.sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", 0)
        self.wait_for_sporks_same()
        self.sync_blocks()
        block_hash = self.generate(producer, 1)[0]
        self.wait_for_chainlocked_block_all_nodes(block_hash)


if __name__ == '__main__':
    LLMQChainLocksAutomaticTest().main()
