#!/usr/bin/env python3
# Copyright (c) 2021-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test for assumeutxo, a means of quickly bootstrapping a node using
a serialized version of the UTXO set at a certain height, which corresponds
to a hash that has been compiled into dashd.

The blocks mined here depend on how long the syncs take, which move the mock
time on, so the nodes that load the snapshot authorize it with -assumeutxodata
instead of an entry in `CRegTestParams::m_assumeutxo_data`.

## Possible test improvements

Interesting test cases could be loading an assumeutxo snapshot file with:

- TODO: Valid hash but invalid snapshot file (bad coin height or
      bad other serialization)
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
from dataclasses import dataclass
from decimal import Decimal
from shutil import rmtree

from test_framework.messages import tx_from_hex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import (
    assert_approx,
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.wallet import (
    getnewdestination,
    MiniWallet,
)

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

    def test_invalid_snapshot_scenarios(self, valid_snapshot_path):
        self.log.info("Test different scenarios of loading invalid snapshot files")
        with open(valid_snapshot_path, 'rb') as f:
            valid_snapshot_contents = f.read()
        bad_snapshot_path = valid_snapshot_path + '.mod'

        def expected_error(log_msg="", rpc_details=""):
            with self.nodes[1].assert_debug_log([log_msg]):
                assert_raises_rpc_error(-32603, f"Unable to load UTXO snapshot{rpc_details}", self.nodes[1].loadtxoutset, bad_snapshot_path)

        self.log.info("  - snapshot file refering to a block that is not in the assumeutxo parameters")
        prev_block_hash = self.nodes[0].getblockhash(SNAPSHOT_BASE_HEIGHT - 1)
        bogus_block_hash = "0" * 64  # Represents any unknown block hash
        for bad_block_hash in [bogus_block_hash, prev_block_hash]:
            with open(bad_snapshot_path, 'wb') as f:
                # block hash of the snapshot base is stored right at the start (first 32 bytes)
                f.write(bytes.fromhex(bad_block_hash)[::-1] + valid_snapshot_contents[32:])
            error_details = f", assumeutxo block hash in snapshot metadata not recognized ({bad_block_hash})"
            expected_error(rpc_details=error_details)

        self.log.info("  - snapshot file with wrong number of coins")
        valid_num_coins = int.from_bytes(valid_snapshot_contents[32:32 + 8], "little")
        for off in [-1, +1]:
            with open(bad_snapshot_path, 'wb') as f:
                f.write(valid_snapshot_contents[:32])
                f.write((valid_num_coins + off).to_bytes(8, "little"))
                f.write(valid_snapshot_contents[32 + 8:])
            # The evo section follows the coins, so one coin too many is read from it.
            expected_error(log_msg=f"bad snapshot - coins left over after deserializing {valid_num_coins - 1} coins" if off == -1 else f"after deserializing {valid_num_coins} coins")

        self.log.info("  - snapshot file with alternated UTXO data")
        cases = [
            [b"\xff" * 32, 0], # wrong outpoint hash
            [(1).to_bytes(4, "little"), 32], # wrong outpoint index
        ]
        # The first coin differs between runs (see authorize_snapshot), so its code
        # VARINT((coinbase ? 1 : 0) | (height << 1)) is changed in its last byte,
        # which keeps its length, and only in the coinbase flag and the lowest bit
        # of the height, which keeps the height at or below the base.
        code_end = 36
        while valid_snapshot_contents[32 + 8 + code_end] & 0x80:
            code_end += 1
        code_last = valid_snapshot_contents[32 + 8 + code_end]
        cases += [
            [bytes([code_last ^ 1]), code_end], # wrong coin code: coinbase flag
            [bytes([code_last ^ 2]), code_end], # another wrong coin code: height
        ]

        for content, offset in cases:
            with open(bad_snapshot_path, "wb") as f:
                f.write(valid_snapshot_contents[:(32 + 8 + offset)])
                f.write(content)
                f.write(valid_snapshot_contents[(32 + 8 + offset + len(content)):])
            expected_error(log_msg=f"[snapshot] bad snapshot content hash: expected {self.snapshot_hash}, got ")

    def test_invalid_chainstate_scenarios(self):
        self.log.info("Test different scenarios of invalid snapshot chainstate in datadir")

        self.log.info("  - snapshot chainstate refering to a block that is not in the assumeutxo parameters")
        self.stop_node(0)
        chainstate_snapshot_path = self.nodes[0].chain_path / "chainstate_snapshot"
        chainstate_snapshot_path.mkdir()
        with open(chainstate_snapshot_path / "base_blockhash", 'wb') as f:
            f.write(b'z' * 32)

        def expected_error(log_msg="", error_msg=""):
            with self.nodes[0].assert_debug_log([log_msg]):
                self.nodes[0].assert_start_raises_init_error(expected_msg=error_msg, match=ErrorMatch.PARTIAL_REGEX)

        # The node has no EvoDB state for that snapshot either, which startup
        # reports first, with the prompt to reindex.
        expected_error_msg = "Snapshot chainstate EvoDB marker is missing. Reindex is required."
        error_details = "snapshot EvoDB marker is missing for base block"
        expected_error(log_msg=error_details, error_msg=expected_error_msg)

        # resurrect node again
        rmtree(chainstate_snapshot_path)
        self.start_node(0)

    def test_invalid_file_path(self):
        self.log.info("Test dashd should fail when file path is invalid.")
        node = self.nodes[0]
        path = node.datadir_path / node.chain / "invalid" / "path"
        assert_raises_rpc_error(-8, "Couldn't open file {} for reading.".format(path), node.loadtxoutset, path)

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
        assert n0.getblockcount() == START_HEIGHT
        blocks = {START_HEIGHT: Block(n0.getbestblockhash(), 1, START_HEIGHT + 1)}
        for i in range(100):
            block_tx = 1
            if START_HEIGHT + 1 + i in EXTRA_TX_HEIGHTS:
                mini_wallet.send_self_transfer(from_node=n0)
                block_tx += 1
            self.generate(n0, nblocks=1, sync_fun=self.no_op)
            height = n0.getblockcount()
            hash = n0.getbestblockhash()
            blocks[height] = Block(hash, block_tx, blocks[height-1].chain_tx + block_tx)
            newblock = n0.getblock(n0.getbestblockhash(), 0)
            if i == 4:
                # Create a stale block that forks off the main chain before the snapshot.
                temp_invalid = n0.getbestblockhash()
                n0.invalidateblock(temp_invalid)
                stale_hash = self.generateblock(n0, output="raw(aaaa)", transactions=[], sync_fun=self.no_op)["hash"]
                n0.invalidateblock(stale_hash)
                n0.reconsiderblock(temp_invalid)
                stale_block = n0.getblock(stale_hash, 0)

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
        assert_equal(dump_output['nchaintx'], blocks[SNAPSHOT_BASE_HEIGHT].chain_tx)
        self.authorize_snapshot(dump_output)
        assert_equal(n0.getblockchaininfo()["blocks"], SNAPSHOT_BASE_HEIGHT)

        # Mine more blocks on top of the snapshot that n1 hasn't yet seen. This
        # will allow us to test n1's sync-to-tip on top of a snapshot.
        self.generate(n0, nblocks=100, sync_fun=self.no_op)

        assert_equal(n0.getblockcount(), FINAL_HEIGHT)
        assert_equal(n1.getblockcount(), START_HEIGHT)

        assert_equal(n0.getblockchaininfo()["blocks"], FINAL_HEIGHT)

        self.test_invalid_snapshot_scenarios(dump_output['path'])
        self.test_invalid_chainstate_scenarios()
        self.test_invalid_file_path()

        self.log.info(f"Loading snapshot into second node from {dump_output['path']}")
        loaded = n1.loadtxoutset(dump_output['path'])
        assert_equal(loaded['coins_loaded'], dump_output['coins_written'])
        assert_equal(loaded['base_height'], SNAPSHOT_BASE_HEIGHT)

        def check_tx_counts(final: bool) -> None:
            """Check nTx and nChainTx intermediate values right after loading
            the snapshot, and final values after the snapshot is validated."""
            for height, block in blocks.items():
                tx = n1.getblockheader(block.hash)["nTx"]
                stats = n1.getchaintxstats(nblocks=1, blockhash=block.hash)
                chain_tx = stats.get("txcount", None)
                window_tx_count = stats.get("window_tx_count", None)
                tx_rate = stats.get("txrate", None)
                window_interval = stats.get("window_interval")

                # Intermediate nTx of the starting block should be set, but nTx of
                # later blocks should be 0 before they are downloaded.
                # The window_tx_count of one block is equal to the blocks tx count.
                # If the window tx count is unknown, the value is missing.
                # The tx_rate is calculated from window_tx_count and window_interval
                # when possible.
                if final or height == START_HEIGHT:
                    assert_equal(tx, block.tx)
                    assert_equal(window_tx_count, tx)
                    if window_interval > 0:
                        assert_approx(tx_rate, window_tx_count / window_interval, vspan=0.1)
                    else:
                        assert_equal(tx_rate, None)
                else:
                    assert_equal(tx, 0)
                    assert_equal(window_tx_count, None)

                # Intermediate nChainTx of the starting block and snapshot block
                # should be set, but others should be None until they are downloaded.
                if final or height in (START_HEIGHT, SNAPSHOT_BASE_HEIGHT):
                    assert_equal(chain_tx, block.chain_tx)
                else:
                    assert_equal(chain_tx, None)

        check_tx_counts(final=False)

        normal, snapshot = n1.getchainstates()["chainstates"]
        assert_equal(normal['blocks'], START_HEIGHT)
        assert_equal(normal.get('snapshot_blockhash'), None)
        assert_equal(normal['validated'], True)
        assert_equal(snapshot['blocks'], SNAPSHOT_BASE_HEIGHT)
        assert_equal(snapshot['snapshot_blockhash'], dump_output['base_hash'])
        assert_equal(snapshot['validated'], False)

        assert_equal(n1.getblockchaininfo()["blocks"], SNAPSHOT_BASE_HEIGHT)

        self.log.info("Submit a stale block that forked off the chain before the snapshot")
        # Normally a block like this would not be downloaded, but if it is
        # submitted early before the background chain catches up to the fork
        # point, it winds up in m_blocks_unlinked and triggers a corner case
        # that previously crashed CheckBlockIndex.
        n1.submitblock(stale_block)
        n1.getchaintips()
        n1.getblock(stale_hash)

        self.log.info("Submit a spending transaction for a snapshot chainstate coin to the mempool")
        # spend the coinbase output of the first block that is not available on node1
        spend_coin_blockhash = n1.getblockhash(START_HEIGHT + 1)
        assert_raises_rpc_error(-1, "Block not found on disk", n1.getblock, spend_coin_blockhash)
        prev_tx = n0.getblock(spend_coin_blockhash, 3)['tx'][0]
        prevout = {"txid": prev_tx['txid'], "vout": 0, "scriptPubKey": prev_tx['vout'][0]['scriptPubKey']['hex']}
        privkey = n0.get_deterministic_priv_key().key
        raw_tx = n1.createrawtransaction([prevout], {getnewdestination()[2]: prev_tx['vout'][0]['value'] - Decimal("0.001")})
        signed_tx = n1.signrawtransactionwithkey(raw_tx, [privkey], [prevout])['hex']
        signed_txid = tx_from_hex(signed_tx).rehash()

        assert n1.gettxout(prev_tx['txid'], 0) is not None
        n1.sendrawtransaction(signed_tx)
        assert signed_txid in n1.getrawmempool()
        assert not n1.gettxout(prev_tx['txid'], 0)

        PAUSE_HEIGHT = FINAL_HEIGHT - 40

        self.log.info("Restarting node to stop at height %d", PAUSE_HEIGHT)
        self.restart_node(1, extra_args=[
            f"-stopatheight={PAUSE_HEIGHT}", *self.extra_args[1]])

        # Finally connect the nodes and let them sync.
        #
        # Set `wait_for_connect=False` to avoid a race between performing connection
        # assertions and the -stopatheight tripping.
        self.connect_nodes(0, 1, wait_for_connect=False)

        n1.wait_until_stopped(timeout=5)

        self.log.info("Restarted node before snapshot validation completed, reloading...")
        self.restart_node(1, extra_args=self.extra_args[1])

        # Send snapshot block to n1 out of order. This makes the test less
        # realistic because normally the snapshot block is one of the last
        # blocks downloaded, but its useful to test because it triggers more
        # corner cases in ReceivedBlockTransactions() and CheckBlockIndex()
        # setting and testing nChainTx values, and it exposed previous bugs.
        snapshot_hash = n0.getblockhash(SNAPSHOT_BASE_HEIGHT)
        snapshot_block = n0.getblock(snapshot_hash, 0)
        n1.submitblock(snapshot_block)

        self.connect_nodes(0, 1)

        self.log.info(f"Ensuring snapshot chain syncs to tip. ({FINAL_HEIGHT})")
        self.wait_until(lambda: n1.getchainstates()['chainstates'][-1]['blocks'] == FINAL_HEIGHT)
        self.sync_blocks(nodes=(n0, n1))

        self.log.info("Ensuring background validation completes")
        self.wait_until(lambda: len(n1.getchainstates()['chainstates']) == 1)

        # Ensure indexes have synced. Dash nodes keep txindex and the governance
        # collateral index by default.
        completed_idx_state = {
            'basic block filter index': COMPLETE_IDX,
            'coinstatsindex': COMPLETE_IDX,
            'govcollateralindex': COMPLETE_IDX,
            'txindex': COMPLETE_IDX,
        }
        self.wait_until(lambda: n1.getindexinfo() == completed_idx_state)

        self.log.info("Re-check nTx and nChainTx values")
        check_tx_counts(final=True)

        for i in (0, 1):
            n = self.nodes[i]
            self.log.info(f"Restarting node {i} to ensure (Check|Load)BlockIndex passes")
            self.restart_node(i, extra_args=self.extra_args[i])

            assert_equal(n.getblockchaininfo()["blocks"], FINAL_HEIGHT)

            chainstate, = n.getchainstates()['chainstates']
            assert_equal(chainstate['blocks'], FINAL_HEIGHT)

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

        for reindex_arg in ['-reindex=1', '-reindex-chainstate=1']:
            self.log.info(f"Check that restarting with {reindex_arg} will delete the snapshot chainstate")
            self.restart_node(2, extra_args=[reindex_arg, *self.extra_args[2]])
            assert_equal(1, len(n2.getchainstates()["chainstates"]))
            for i in range(1, 300):
                block = n0.getblock(n0.getblockhash(i), 0)
                n2.submitheader(block)
            loaded = n2.loadtxoutset(dump_output['path'])
            assert_equal(loaded['coins_loaded'], dump_output['coins_written'])
            assert_equal(loaded['base_height'], SNAPSHOT_BASE_HEIGHT)

        normal, snapshot = n2.getchainstates()['chainstates']
        assert_equal(normal['blocks'], START_HEIGHT)
        assert_equal(normal.get('snapshot_blockhash'), None)
        assert_equal(normal['validated'], True)
        assert_equal(snapshot['blocks'], SNAPSHOT_BASE_HEIGHT)
        assert_equal(snapshot['snapshot_blockhash'], dump_output['base_hash'])
        assert_equal(snapshot['validated'], False)

        self.connect_nodes(0, 2)
        self.wait_until(lambda: n2.getchainstates()['chainstates'][-1]['blocks'] == FINAL_HEIGHT)
        self.sync_blocks(nodes=(n0, n2))

        self.log.info("Ensuring background validation completes")
        self.wait_until(lambda: len(n2.getchainstates()['chainstates']) == 1)

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

            chainstate, = n.getchainstates()['chainstates']
            assert_equal(chainstate['blocks'], FINAL_HEIGHT)

            if i != 0:
                # Ensure indexes have synced for the assumeutxo node
                self.wait_until(lambda: n.getindexinfo() == completed_idx_state)

        self.log.info("Test -reindex-chainstate of an assumeutxo-synced node")
        self.restart_node(2, extra_args=[
            '-reindex-chainstate=1', *self.extra_args[2]])
        assert_equal(n2.getblockchaininfo()["blocks"], FINAL_HEIGHT)
        self.wait_until(lambda: n2.getblockcount() == FINAL_HEIGHT)

        self.log.info("Test -reindex of an assumeutxo-synced node")
        self.restart_node(2, extra_args=['-reindex=1', *self.extra_args[2]])
        self.connect_nodes(0, 2)
        self.wait_until(lambda: n2.getblockcount() == FINAL_HEIGHT)

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
        chainstate, = n3.getchainstates()['chainstates']
        assert_equal(chainstate.get('snapshot_blockhash'), None)
        assert not (n3.chain_path / "chainstate_snapshot").exists()
        self.connect_nodes(0, 3)
        self.wait_until(lambda: n3.getblockcount() == FINAL_HEIGHT)

        self.log.info("Restarting node 3 to ensure (Check|Load)BlockIndex passes")
        self.restart_node(3, extra_args=self.extra_args[3])
        assert_equal(n3.getblockcount(), FINAL_HEIGHT)


@dataclass
class Block:
    hash: str
    tx: int
    chain_tx: int


if __name__ == '__main__':
    AssumeutxoTest().main()
