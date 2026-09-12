#!/usr/bin/env python3
# Copyright (c) 2017-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the wallet resends transactions periodically."""

from test_framework.blocktools import (
    create_block,
    create_coinbase,
)
from test_framework.messages import DEFAULT_MEMPOOL_EXPIRY_HOURS
from test_framework.p2p import P2PTxInvStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

class ResendWalletTransactionsTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]  # alias

        peer_first = node.add_p2p_connection(P2PTxInvStore())

        self.log.info("Create a new transaction and wait until it's broadcast")
        parent_utxo, indep_utxo = node.listunspent()[:2]
        addr = node.getnewaddress()
        txid = node.send(outputs=[{addr: 1}], options={"inputs": [parent_utxo]})["txid"]

        # Can take a few seconds due to transaction trickling
        def wait_p2p():
            self.bump_mocktime(1)
            return peer_first.tx_invs_received[int(txid, 16)] >= 1
        self.wait_until(wait_p2p)

        # Add a second peer since txs aren't rebroadcast to the same peer (see m_tx_inventory_known_filter)
        peer_second = node.add_p2p_connection(P2PTxInvStore())

        self.log.info("Create a block")
        # Create and submit a block without the transaction.
        # Transactions are only rebroadcast if there has been a block at least five minutes
        # after the last time we tried to broadcast. Use mocktime and give an extra minute to be sure.
        block_time = self.mocktime + 6 * 60
        node.setmocktime(block_time)
        block = create_block(int(node.getbestblockhash(), 16), create_coinbase(node.getblockcount() + 1), block_time)
        block.solve()
        node.submitblock(block.serialize().hex())

        # Set correct m_best_block_time, which is used in ResubmitWalletTransactions
        node.syncwithvalidationinterfacequeue()

        # Transaction should not be rebroadcast within first hour
        # Leave 2 mins for buffer
        one_hour = 60 * 60
        two_min = 2 * 60
        node.setmocktime(self.mocktime + one_hour - two_min)
        node.mockscheduler(60)  # Tell scheduler to call MaybeResendWalletTxs now
        assert_equal(int(txid, 16) in peer_second.get_invs(), False)

        self.log.info("Bump time & check that transaction is rebroadcast")
        # Transaction should be rebroadcast approximately 2 hours in the future,
        # but can range from 1-3. So bump 3 hours to be sure.
        with node.assert_debug_log(['resubmit 1 unconfirmed transactions']):
            node.setmocktime(self.mocktime + 3 * 60 * 60)
            # Tell scheduler to call MaybeResendWalletTxs now.
            node.mockscheduler(60)
        # Give some time for trickle to occur
        node.setmocktime(self.mocktime + 36 * 60 * 60 + 600)
        peer_second.wait_for_broadcast([txid])

        self.log.info("Chain of unconfirmed not-in-mempool txs are rebroadcast")
        # This tests that the node broadcasts the parent transaction before the child transaction.
        # To test that scenario, we need a method to reliably get a child transaction placed
        # in mapWallet positioned before the parent. We cannot predict the position in mapWallet,
        # but we can observe it using listreceivedbyaddress and other related RPCs.
        #
        # So we will create the child transaction, then use listreceivedbyaddress to see what the
        # ordering of mapWallet is. mapWallet hashes txids with a salt drawn at random every time
        # the wallet is loaded, so reloading the wallet reshuffles its iteration order while
        # leaving the transactions themselves untouched. Reload until the child comes first.
        child_txid = node.send(outputs=[{addr: 0.5}], options={"inputs": [{"txid":txid, "vout":0}]})["txid"]
        while True:
            txids = node.listreceivedbyaddress(minconf=0, address_filter=addr)[0]["txids"]
            if txids == [child_txid, txid]:
                break
            node.unloadwallet(self.default_wallet_name)
            node.loadwallet(self.default_wallet_name)
        entry_time = node.getmempoolentry(child_txid)["time"]

        block_time = entry_time + 6 * 60
        node.setmocktime(block_time)
        block = create_block(int(node.getbestblockhash(), 16), create_coinbase(node.getblockcount() + 1), block_time)
        block.solve()
        node.submitblock(block.serialize().hex())
        # Set correct m_best_block_time, which is used in ResubmitWalletTransactions
        node.syncwithvalidationinterfacequeue()

        # Evict these txs from the mempool
        evict_time = block_time + 60 * 60 * DEFAULT_MEMPOOL_EXPIRY_HOURS + 5
        node.setmocktime(evict_time)
        indep_send = node.send(outputs=[{node.getnewaddress(): 1}], options={"inputs": [indep_utxo]})
        node.getmempoolentry(indep_send["txid"])
        assert_raises_rpc_error(-5, "Transaction not in mempool", node.getmempoolentry, txid)
        assert_raises_rpc_error(-5, "Transaction not in mempool", node.getmempoolentry, child_txid)

        # Rebroadcast and check that parent and child are both in the mempool
        with node.assert_debug_log(['resubmit 2 unconfirmed transactions']):
            node.setmocktime(evict_time + 36 * 60 * 60) # 36 hrs is the upper limit of the resend timer
            node.mockscheduler(60)
        node.getmempoolentry(txid)
        node.getmempoolentry(child_txid)


if __name__ == '__main__':
    ResendWalletTransactionsTest().main()
