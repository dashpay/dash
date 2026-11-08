#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test for assumeutxo, a means of quickly bootstrapping a node using
a serialized version of the UTXO set at a certain height, which corresponds
to a hash that has been compiled into dashd.

The blocks mined here depend on how long the syncs take, which move the mock
time on, so the nodes that load the snapshot authorize it with -assumeutxodata
instead of an entry in `CRegTestParams::m_assumeutxo_data`.

## Possible test improvements

- TODO: test submitting a transaction and verifying it appears in mempool
- TODO: test what happens with -reindex and -reindex-chainstate before the
      snapshot is validated, and make sure it's deleted successfully.

Interesting test cases could be loading an assumeutxo snapshot file with:

- TODO: An invalid hash
- TODO: Valid hash but invalid snapshot file (bad coin height or truncated file or
      bad other serialization)
- TODO: Valid snapshot file, but referencing an unknown block
- TODO: Valid snapshot file, but referencing a snapshot block that turns out to be
      invalid, or has an invalid parent
- TODO: Valid snapshot file and snapshot block, but the block is not on the
      most-work chain

Interesting starting states could be loading a snapshot when the current chain tip is:

- TODO: An ancestor of snapshot block
- TODO: Not an ancestor of the snapshot block but has less work
- TODO: The snapshot block
- TODO: A descendant of the snapshot block
- TODO: Not an ancestor or a descendant of the snapshot block and has more work

"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, wait_until_helper
from test_framework.wallet import MiniWallet

START_HEIGHT = 199
EXTRA_TX_HEIGHTS = (210, 220)
SNAPSHOT_BASE_HEIGHT = 299
FINAL_HEIGHT = 399
COMPLETE_IDX = {'synced': True, 'best_block_height': FINAL_HEIGHT}


class AssumeutxoTest(BitcoinTestFramework):

    def set_test_params(self):
        """Use the pregenerated, deterministic chain up to height 199."""
        self.num_nodes = 4
        self.rpc_timeout = 120
        # A snapshot chainstate cannot run in prune mode.
        self.extra_args = [
            [],
            ["-blockfilterindex=1", "-coinstatsindex=1"],
            ["-txindex=1", "-blockfilterindex=1", "-coinstatsindex=1"],
            [],
        ]

    def setup_network(self):
        """Start with the nodes disconnected so that one can generate a snapshot
        including blocks the other hasn't yet seen."""
        self.add_nodes(4)
        self.start_nodes(extra_args=self.extra_args)

    def authorize_snapshot(self, dump_output):
        """Restart the nodes that load the snapshot with it authorized: the
        regtest chain parameters have no entry for this chain."""
        self.snapshot_hash = dump_output['txoutset_hash']
        pin = f"-assumeutxodata={dump_output['base_height']}:{self.snapshot_hash}:{dump_output['evo']['hash']}:{dump_output['nchaintx']}:{dump_output['base_hash']}"
        for i in (1, 2, 3):
            self.extra_args[i].append(pin)
            self.restart_node(i, extra_args=self.extra_args[i])

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
        n3 = self.nodes[3]

        # Mock time for a deterministic chain
        for n in self.nodes:
            n.setmocktime(n.getblockheader(n.getbestblockhash())['time'])

        self.sync_blocks()

        def no_sync():
            pass

        # Generate a series of blocks that `n0` will have in the snapshot,
        # but that n1 doesn't yet see. In order for the snapshot to activate,
        # though, we have to ferry over the new headers to n1 so that it
        # isn't waiting forever to see the header of the snapshot's base block
        # while disconnected from n0.
        #
        # Two of these blocks carry a transaction besides the coinbase. The
        # loading nodes count one transaction for each block they lack until
        # they download it.
        mini_wallet = MiniWallet(n0)
        for i in range(100):
            if START_HEIGHT + 1 + i in EXTRA_TX_HEIGHTS:
                mini_wallet.send_self_transfer(from_node=n0)
            self.generate(n0, nblocks=1, sync_fun=no_sync)
            newblock = n0.getblock(n0.getbestblockhash(), 0)

            # make n1 aware of the new header, but don't give it the block.
            n1.submitheader(newblock)
            n2.submitheader(newblock)
            n3.submitheader(newblock)

        # Ensure everyone is seeing the same headers.
        for n in self.nodes:
            assert_equal(n.getblockchaininfo()["headers"], SNAPSHOT_BASE_HEIGHT)
        for height in EXTRA_TX_HEIGHTS:
            assert_equal(len(n0.getblock(n0.getblockhash(height))['tx']), 2)

        self.log.info("-- Testing assumeutxo + some indexes")

        assert_equal(n0.getblockcount(), SNAPSHOT_BASE_HEIGHT)
        assert_equal(n1.getblockcount(), START_HEIGHT)

        self.log.info(f"Creating a UTXO snapshot at height {SNAPSHOT_BASE_HEIGHT}")
        dump_output = n0.dumptxoutset('utxos.dat')

        # The blocks depend on how long the syncs took, which move the mock time
        # on, so the nodes that load the snapshot authorize this dump.
        assert_equal(dump_output['nchaintx'], n0.getchaintxstats(blockhash=dump_output['base_hash'])['txcount'])
        self.authorize_snapshot(dump_output)
        assert_equal(n0.getblockchaininfo()["blocks"], SNAPSHOT_BASE_HEIGHT)

        # Mine more blocks on top of the snapshot that n1 hasn't yet seen. This
        # will allow us to test n1's sync-to-tip on top of a snapshot.
        self.generate(n0, nblocks=100, sync_fun=no_sync)

        assert_equal(n0.getblockcount(), FINAL_HEIGHT)
        assert_equal(n1.getblockcount(), START_HEIGHT)

        assert_equal(n0.getblockchaininfo()["blocks"], FINAL_HEIGHT)

        self.log.info(f"Loading snapshot into second node from {dump_output['path']}")
        loaded = n1.loadtxoutset(dump_output['path'])
        assert_equal(loaded['coins_loaded'], dump_output['coins_written'])
        assert_equal(loaded['base_height'], SNAPSHOT_BASE_HEIGHT)

        monitor = n1.getchainstates()
        assert_equal(monitor['normal']['blocks'], START_HEIGHT)
        assert_equal(monitor['snapshot']['blocks'], SNAPSHOT_BASE_HEIGHT)
        assert_equal(monitor['snapshot']['snapshot_blockhash'], dump_output['base_hash'])

        assert_equal(n1.getblockchaininfo()["blocks"], SNAPSHOT_BASE_HEIGHT)

        PAUSE_HEIGHT = FINAL_HEIGHT - 40

        self.log.info("Restarting node to stop at height %d", PAUSE_HEIGHT)
        self.restart_node(1, extra_args=[
            f"-stopatheight={PAUSE_HEIGHT}", *self.extra_args[1]])

        # Finally connect the nodes and let them sync.
        self.connect_nodes(0, 1)

        n1.wait_until_stopped(timeout=5)

        self.log.info("Restarted node before snapshot validation completed, reloading...")
        self.restart_node(1, extra_args=self.extra_args[1])
        self.connect_nodes(0, 1)

        self.log.info(f"Ensuring snapshot chain syncs to tip. ({FINAL_HEIGHT})")
        wait_until_helper(lambda: n1.getchainstates()['snapshot']['blocks'] == FINAL_HEIGHT)
        self.sync_blocks(nodes=(n0, n1))

        self.log.info("Ensuring background validation completes")
        # N.B.: the `snapshot` key disappears once the background validation is complete.
        wait_until_helper(lambda: not n1.getchainstates().get('snapshot'))

        # Ensure indexes have synced. Dash nodes keep txindex and the governance
        # collateral index by default.
        completed_idx_state = {
            'basic block filter index': COMPLETE_IDX,
            'coinstatsindex': COMPLETE_IDX,
            'govcollateralindex': COMPLETE_IDX,
            'txindex': COMPLETE_IDX,
        }
        self.wait_until(lambda: n1.getindexinfo() == completed_idx_state)


        for i in (0, 1):
            n = self.nodes[i]
            self.log.info(f"Restarting node {i} to ensure (Check|Load)BlockIndex passes")
            self.restart_node(i, extra_args=self.extra_args[i])

            assert_equal(n.getblockchaininfo()["blocks"], FINAL_HEIGHT)

            assert_equal(n.getchainstates()['normal']['blocks'], FINAL_HEIGHT)
            assert_equal(n.getchainstates().get('snapshot'), None)

            if i != 0:
                # Ensure indexes have synced for the assumeutxo node
                self.wait_until(lambda: n.getindexinfo() == completed_idx_state)


        # Node 2: all indexes + reindex
        # -----------------------------

        self.log.info("-- Testing all indexes + reindex")
        assert_equal(n2.getblockcount(), START_HEIGHT)

        self.log.info(f"Loading snapshot into third node from {dump_output['path']}")
        loaded = n2.loadtxoutset(dump_output['path'])
        assert_equal(loaded['coins_loaded'], dump_output['coins_written'])
        assert_equal(loaded['base_height'], SNAPSHOT_BASE_HEIGHT)

        monitor = n2.getchainstates()
        assert_equal(monitor['normal']['blocks'], START_HEIGHT)
        assert_equal(monitor['snapshot']['blocks'], SNAPSHOT_BASE_HEIGHT)
        assert_equal(monitor['snapshot']['snapshot_blockhash'], dump_output['base_hash'])

        self.connect_nodes(0, 2)
        wait_until_helper(lambda: n2.getchainstates()['snapshot']['blocks'] == FINAL_HEIGHT)
        self.sync_blocks(nodes=(n0, n2))

        self.log.info("Ensuring background validation completes")
        wait_until_helper(lambda: not n2.getchainstates().get('snapshot'))

        completed_idx_state = {
            'basic block filter index': COMPLETE_IDX,
            'coinstatsindex': COMPLETE_IDX,
            'govcollateralindex': COMPLETE_IDX,
            'txindex': COMPLETE_IDX,
        }
        self.wait_until(lambda: n2.getindexinfo() == completed_idx_state)

        for i in (0, 2):
            n = self.nodes[i]
            self.log.info(f"Restarting node {i} to ensure (Check|Load)BlockIndex passes")
            self.restart_node(i, extra_args=self.extra_args[i])

            assert_equal(n.getblockchaininfo()["blocks"], FINAL_HEIGHT)

            assert_equal(n.getchainstates()['normal']['blocks'], FINAL_HEIGHT)
            assert_equal(n.getchainstates().get('snapshot'), None)

            if i != 0:
                # Ensure indexes have synced for the assumeutxo node
                self.wait_until(lambda: n.getindexinfo() == completed_idx_state)

        self.log.info("Test -reindex-chainstate of an assumeutxo-synced node")
        self.restart_node(2, extra_args=[
            '-reindex-chainstate=1', *self.extra_args[2]])
        assert_equal(n2.getblockchaininfo()["blocks"], FINAL_HEIGHT)
        wait_until_helper(lambda: n2.getblockcount() == FINAL_HEIGHT)

        self.log.info("Test -reindex of an assumeutxo-synced node")
        self.restart_node(2, extra_args=['-reindex=1', *self.extra_args[2]])
        self.connect_nodes(0, 2)
        wait_until_helper(lambda: n2.getblockcount() == FINAL_HEIGHT)

        # Node 3: -reindex-chainstate before the snapshot is validated
        # ------------------------------------------------------------

        self.log.info("-- Testing -reindex-chainstate before the snapshot is validated")
        assert_equal(n3.getblockcount(), START_HEIGHT)
        loaded = n3.loadtxoutset(dump_output['path'])
        assert_equal(loaded['base_height'], SNAPSHOT_BASE_HEIGHT)
        # The snapshot chainstate connects the blocks above the base, and with no
        # peer the background chainstate gets none below it.
        for height in range(SNAPSHOT_BASE_HEIGHT + 1, FINAL_HEIGHT + 1):
            n3.submitblock(n0.getblock(n0.getblockhash(height), 0))
        assert_equal(n3.getblockcount(), FINAL_HEIGHT)

        self.log.info("-reindex-chainstate drops the snapshot, and the node syncs normally")
        self.restart_node(3, extra_args=['-reindex-chainstate=1', *self.extra_args[3]])
        assert_equal(n3.getchainstates().get('snapshot'), None)
        assert not (n3.chain_path / "chainstate_snapshot").exists()
        self.connect_nodes(0, 3)
        self.wait_until(lambda: n3.getblockcount() == FINAL_HEIGHT)

        self.log.info("Restarting node 3 to ensure (Check|Load)BlockIndex passes")
        self.restart_node(3, extra_args=self.extra_args[3])
        assert_equal(n3.getblockcount(), FINAL_HEIGHT)


if __name__ == '__main__':
    AssumeutxoTest().main()
