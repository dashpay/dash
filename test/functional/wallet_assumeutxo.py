#!/usr/bin/env python3
# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test for assumeutxo wallet related behavior.
See feature_assumeutxo.py for background.

## Possible test improvements

- TODO: test loading a wallet (backup) on a pruned node

"""
from test_framework.descriptors import descsum_create
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    dumb_sync_blocks,
)
from test_framework.wallet_util import get_generate_key

START_HEIGHT = 199
SNAPSHOT_BASE_HEIGHT = 299
FINAL_HEIGHT = 399


class AssumeutxoTest(BitcoinTestFramework):
    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        """Use the pregenerated, deterministic chain up to height 199."""
        self.num_nodes = 3
        self.rpc_timeout = 120
        self.extra_args = [
            [],
            [],
            [],
        ]

    def setup_network(self):
        """Start with the nodes disconnected so that one can generate a snapshot
        including blocks the other hasn't yet seen."""
        self.add_nodes(3)
        self.start_nodes(extra_args=self.extra_args)

    def import_descriptor(self, node, wallet_name, key, timestamp):
        import_request = [{"desc": descsum_create("pkh(" + key.pubkey + ")"),
                           "timestamp": timestamp,
                           "label": "Descriptor import test"}]
        wrpc = node.get_wallet_rpc(wallet_name)
        return wrpc.importdescriptors(import_request)

    def run_test(self):
        """
        Bring up two (disconnected) nodes, mine some new blocks on the first,
        and generate a UTXO snapshot.

        Load the snapshot into the second, ensure it syncs to tip and completes
        background validation when connected to the first.
        """
        n0 = self.nodes[0]
        n1 = self.nodes[1]
        n2 = self.nodes[2]

        # Mock time for a deterministic chain
        for n in self.nodes:
            n.setmocktime(n.getblockheader(n.getbestblockhash())['time'])

        self.sync_blocks()

        # Create a wallet that we will create a backup for later (at snapshot height)
        n0.createwallet('w')
        w = n0.get_wallet_rpc("w")

        # Create another wallet and backup now (before snapshot height)
        n0.createwallet('w2')
        w2 = n0.get_wallet_rpc("w2")
        w2.backupwallet("backup_w2.dat")

        # Generate a series of blocks that `n0` will have in the snapshot,
        # but that n1 doesn't yet see. In order for the snapshot to activate,
        # though, we have to ferry over the new headers to n1 so that it
        # isn't waiting forever to see the header of the snapshot's base block
        # while disconnected from n0.
        for _ in range(100):
            self.generate(n0, nblocks=1, sync_fun=self.no_op)
            newblock = n0.getblock(n0.getbestblockhash(), 0)

            # make n1 aware of the new header, but don't give it the block.
            n1.submitheader(newblock)
            n2.submitheader(newblock)

        # Ensure everyone is seeing the same headers.
        for n in self.nodes:
            assert_equal(n.getblockchaininfo()[
                         "headers"], SNAPSHOT_BASE_HEIGHT)

        # This backup is created at the snapshot height, so it's
        # not part of the background sync anymore
        w.backupwallet("backup_w.dat")

        self.log.info("-- Testing assumeutxo")

        assert_equal(n0.getblockcount(), SNAPSHOT_BASE_HEIGHT)
        assert_equal(n1.getblockcount(), START_HEIGHT)

        self.log.info(
            f"Creating a UTXO snapshot at height {SNAPSHOT_BASE_HEIGHT}")
        dump_output = n0.dumptxoutset('utxos.dat')

        # The blocks depend on how long the syncs took, which move the mock time
        # on, so nodes 1 and 2 authorize this dump: the regtest chain parameters
        # have no entry for this chain.
        assert_equal(dump_output['nchaintx'], n0.getchaintxstats(blockhash=dump_output['base_hash'])['txcount'])
        for i in (1, 2):
            self.extra_args[i].append(f"-assumeutxodata={dump_output['base_height']}:{dump_output['txoutset_hash']}:{dump_output['evo']['hash']}:{dump_output['nchaintx']}:{dump_output['base_hash']}")
            self.restart_node(i, extra_args=self.extra_args[i])
        assert_equal(n0.getblockchaininfo()["blocks"], SNAPSHOT_BASE_HEIGHT)

        # Mine more blocks on top of the snapshot that n1 hasn't yet seen. This
        # will allow us to test n1's sync-to-tip on top of a snapshot.
        self.generate(n0, nblocks=100, sync_fun=self.no_op)

        assert_equal(n0.getblockcount(), FINAL_HEIGHT)
        assert_equal(n1.getblockcount(), START_HEIGHT)
        assert_equal(n2.getblockcount(), START_HEIGHT)

        assert_equal(n0.getblockchaininfo()["blocks"], FINAL_HEIGHT)

        self.log.info(
            f"Loading snapshot into second node from {dump_output['path']}")
        loaded = n1.loadtxoutset(dump_output['path'])
        assert_equal(loaded['coins_loaded'], dump_output['coins_written'])
        assert_equal(loaded['base_height'], SNAPSHOT_BASE_HEIGHT)

        normal, snapshot = n1.getchainstates()["chainstates"]
        assert_equal(normal['blocks'], START_HEIGHT)
        assert_equal(normal.get('snapshot_blockhash'), None)
        assert_equal(normal['validated'], True)
        assert_equal(snapshot['blocks'], SNAPSHOT_BASE_HEIGHT)
        assert_equal(snapshot['snapshot_blockhash'], dump_output['base_hash'])
        assert_equal(snapshot['validated'], False)

        assert_equal(n1.getblockchaininfo()["blocks"], SNAPSHOT_BASE_HEIGHT)

        self.log.info("Backup from the snapshot height can be loaded during background sync")
        n1.restorewallet("w", "backup_w.dat")

        self.log.info("Backup from before the snapshot height can't be loaded during background sync")

        # Error message for wallets that need blocks before the snapshot height.
        def loading_error(height):
            return f"Wallet loading failed. Error loading wallet. Wallet requires blocks to be downloaded, and software does not currently support loading wallets while blocks are being downloaded out of order when using assumeutxo snapshots. Wallet should be able to load successfully after node sync reaches height {height}"
        # The target height is SNAPSHOT_BASE_HEIGHT because that's when background sync completes.
        assert_raises_rpc_error(-4, loading_error(SNAPSHOT_BASE_HEIGHT), n1.restorewallet, "w2", "backup_w2.dat")

        self.log.info("Test loading descriptors during background sync")
        wallet_name = "w1"
        n1.createwallet(wallet_name, disable_private_keys=True)
        key = get_generate_key()
        time = n1.getblockchaininfo()['time']
        timestamp = 0
        expected_error_message = f"Rescan failed for descriptor with timestamp {timestamp}. There was an error reading a block from time {time}, which is after or within 7200 seconds of key creation, and could contain transactions pertaining to the desc. As a result, transactions and coins using this desc may not appear in the wallet. This error is likely caused by an in-progress assumeutxo background sync. Check logs or getchainstates RPC for assumeutxo background sync progress and try again later."
        result = self.import_descriptor(n1, wallet_name, key, timestamp)
        assert_equal(result[0]['error']['code'], -1)
        assert_equal(result[0]['error']['message'], expected_error_message)

        self.log.info("Test that rescanning blocks from before the snapshot fails when blocks are not available from the background sync yet")
        w1 = n1.get_wallet_rpc(wallet_name)
        assert_raises_rpc_error(-1, "Failed to rescan unavailable blocks likely due to an in-progress assumeutxo background sync. Check logs or getchainstates RPC for assumeutxo background sync progress and try again later.", w1.rescanblockchain, 100)

        PAUSE_HEIGHT = FINAL_HEIGHT - 40

        self.log.info(f"Unload wallets and sync node up to height {PAUSE_HEIGHT}")
        n1.unloadwallet("w")
        n1.unloadwallet(wallet_name)
        dumb_sync_blocks(src=n0, dst=n1, height=PAUSE_HEIGHT)

        self.log.info("Verify node state during background sync")
        # Verify there are still two chainstates (background validation not complete)
        chainstates = n1.getchainstates()['chainstates']
        assert_equal(len(chainstates), 2)
        # The background chainstate should still be at START_HEIGHT
        assert_equal(chainstates[0]['blocks'], START_HEIGHT)
        assert_equal(chainstates[1]["blocks"], PAUSE_HEIGHT)

        # After restart, wallets that existed before cannot be loaded because
        # the wallet loading code checks if required blocks are available for
        # rescanning. During assumeutxo background sync, blocks before the
        # snapshot are not available, so wallet loading fails.
        # After restart, the required height is SNAPSHOT_BASE_HEIGHT + 1 for all wallets.
        assert_raises_rpc_error(-4, loading_error(SNAPSHOT_BASE_HEIGHT + 1), n1.loadwallet, "w")
        assert_raises_rpc_error(-4, loading_error(SNAPSHOT_BASE_HEIGHT + 1), n1.loadwallet, wallet_name)

        # Verify backup from before snapshot height still can't be restored
        assert_raises_rpc_error(-4, loading_error(SNAPSHOT_BASE_HEIGHT + 1), n1.restorewallet, "w2_test", "backup_w2.dat")

        self.connect_nodes(0, 1)

        self.log.info(
            f"Ensuring snapshot chain syncs to tip. ({FINAL_HEIGHT})")
        self.wait_until(lambda: n1.getchainstates()[
                        'chainstates'][-1]['blocks'] == FINAL_HEIGHT)
        self.sync_blocks(nodes=(n0, n1))

        self.log.info("Ensuring background validation completes")
        self.wait_until(lambda: len(n1.getchainstates()['chainstates']) == 1)

        self.log.info("Ensuring wallet can be restored from a backup that was created before the snapshot height")
        n1.restorewallet("w2", "backup_w2.dat")

        self.log.info("Check the last processed block of a wallet that is active during snapshot completion")
        n2.loadtxoutset(dump_output['path'])
        n2.restorewallet("w", "backup_w.dat")
        w = n2.get_wallet_rpc("w")
        # The snapshot chain reaches the tip before the background chainstate can
        # download a block, so that no block of the active chain follows completion.
        for height in range(SNAPSHOT_BASE_HEIGHT + 1, FINAL_HEIGHT + 1):
            n2.submitblock(n0.getblock(n0.getblockhash(height), 0))
        n2.syncwithvalidationinterfacequeue()
        final_block = {'hash': n0.getbestblockhash(), 'height': FINAL_HEIGHT}
        assert_equal(w.getwalletinfo()['lastprocessedblock'], final_block)
        self.connect_nodes(0, 2)
        self.wait_until(lambda: len(n2.getchainstates()['chainstates']) == 1)
        n2.syncwithvalidationinterfacequeue()
        assert_equal(w.getwalletinfo()['lastprocessedblock'], final_block)

        self.log.info("Ensuring descriptors can be loaded after background sync")
        n1.loadwallet(wallet_name)
        result = self.import_descriptor(n1, wallet_name, key, timestamp)
        assert_equal(result[0]['success'], True)


if __name__ == '__main__':
    AssumeutxoTest().main()
