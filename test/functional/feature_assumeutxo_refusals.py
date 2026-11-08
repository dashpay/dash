#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check when a node refuses to load or keep using an assumeutxo snapshot.

A pruned node, a masternode and a node with -addressindex, -spentindex or
-timestampindex cannot load a snapshot. A node with a snapshot chainstate,
validated or not, does not start with -prune, one of those indexes or
-masternodeblsprivkey until a restart has promoted the snapshot.
-assumeutxodata is regtest-only.

Node 1 authorizes node 0's dump with -assumeutxodata.
"""
import shutil

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    write_config,
)

START_HEIGHT = 199
SNAPSHOT_BASE_HEIGHT = 299
FINAL_HEIGHT = 399
PRUNE_REFUSAL = "An assumeutxo snapshot chainstate is in use, which pruning would break."
INDEX_REFUSAL = "An assumeutxo snapshot chainstate is in use, which -addressindex, -spentindex and -timestampindex do not support"
MASTERNODE_REFUSAL = "An assumeutxo snapshot chainstate is in use, which a masternode cannot run on."


class AssumeutxoRefusalsTest(BitcoinTestFramework):
    def set_test_params(self):
        """Use the pregenerated, deterministic chain up to height 199."""
        self.num_nodes = 2
        self.rpc_timeout = 120

    def setup_network(self):
        self.add_nodes(2)
        self.start_nodes()

    def run_test(self):
        n0, n1 = self.nodes
        for n in self.nodes:
            n.setmocktime(n.getblockheader(n.getbestblockhash())['time'])
        self.sync_blocks()

        # n1 learns the headers up to the base, but not the blocks.
        for _ in range(SNAPSHOT_BASE_HEIGHT - START_HEIGHT):
            self.generate(n0, nblocks=1, sync_fun=self.no_op)
            n1.submitheader(n0.getblock(n0.getbestblockhash(), 0))
        dump_output = n0.dumptxoutset('utxos.dat')
        self.snapshot_path = dump_output['path']
        self.base_hash = dump_output['base_hash']
        self.pin = [f"-assumeutxodata={dump_output['base_height']}:{dump_output['txoutset_hash']}:{dump_output['evo']['hash']}:{dump_output['nchaintx']}:{dump_output['base_hash']}"]
        self.generate(n0, nblocks=FINAL_HEIGHT - SNAPSHOT_BASE_HEIGHT, sync_fun=self.no_op)
        self.masternode_args = [f"-masternodeblsprivkey={n0.bls('generate')['secret']}", "-txindex=1"]

        self.test_load_refusals()
        self.test_startup_refusals()
        self.test_assumeutxodata_off_regtest()

    def snapshot_dir(self):
        return self.nodes[1].chain_path / "chainstate_snapshot"

    def test_load_refusals(self):
        n1 = self.nodes[1]
        for extra_args, reason in [
            (["-prune=1", "-fastprune"], "pruning is enabled"),
            (["-prune=945"], "pruning is enabled"),
            (["-addressindex"], "-addressindex, -spentindex or -timestampindex is enabled"),
            (["-spentindex"], "-addressindex, -spentindex or -timestampindex is enabled"),
            (["-timestampindex"], "-addressindex, -spentindex or -timestampindex is enabled"),
            (self.masternode_args, "the node is a masternode"),
        ]:
            self.log.info(f"A node with {' '.join(extra_args)} refuses to load a snapshot")
            self.restart_node(1, extra_args=extra_args + self.pin)
            with n1.assert_debug_log([f"[snapshot] refusing to activate snapshot {self.base_hash}: {reason}"]):
                assert_raises_rpc_error(-32603, "Unable to load UTXO snapshot", n1.loadtxoutset, self.snapshot_path)
            chainstate, = n1.getchainstates()['chainstates']
            assert_equal(chainstate['blocks'], START_HEIGHT)
            assert not self.snapshot_dir().exists()

    def test_startup_refusals(self):
        n0, n1 = self.nodes
        self.restart_node(1, extra_args=self.pin)
        loaded = n1.loadtxoutset(self.snapshot_path)
        assert_equal(loaded['base_height'], SNAPSHOT_BASE_HEIGHT)
        assert self.snapshot_dir().exists()

        self.log.info("A node with an unvalidated snapshot does not start with -prune, a legacy index or as a masternode")
        self.stop_node(1)
        self.assert_startup_refused()

        self.log.info("Indexes check their sync on the background chainstate, which has the blocks")
        with n1.assert_debug_log(expected_msgs=["govcollateralindex is enabled"], unexpected_msgs=["Governance is disabled"], timeout=10):
            self.start_node(1, extra_args=self.pin)
        assert_equal(len(n1.getchainstates()['chainstates']), 2)

        self.log.info("Once validated, the snapshot is promoted at the next restart")
        self.connect_nodes(0, 1)
        self.wait_until(lambda: len(n1.getchainstates()['chainstates']) == 1)
        self.sync_blocks()
        assert self.snapshot_dir().exists()

        self.log.info("A node with a validated snapshot does not start with -prune, a legacy index or as a masternode until it is promoted")
        self.stop_node(1)
        self.assert_startup_refused()

        self.start_node(1, extra_args=self.pin)
        assert not self.snapshot_dir().exists()
        chainstate, = n1.getchainstates()['chainstates']
        assert_equal(chainstate['blocks'], FINAL_HEIGHT)
        assert_equal(chainstate.get('snapshot_blockhash'), None)

        self.log.info("After the promotion, the node starts with -prune")
        self.restart_node(1, extra_args=["-prune=1", "-fastprune"] + self.pin)
        assert_equal(n1.getblockcount(), FINAL_HEIGHT)
        self.restart_node(1, extra_args=self.pin)

    def assert_startup_refused(self):
        n1 = self.nodes[1]
        for extra_args, error in [
            (["-prune=1", "-fastprune"], PRUNE_REFUSAL),
            (["-prune=945"], PRUNE_REFUSAL),
            (["-addressindex"], INDEX_REFUSAL),
            (["-spentindex"], INDEX_REFUSAL),
            (["-timestampindex"], INDEX_REFUSAL),
            (self.masternode_args, MASTERNODE_REFUSAL),
        ]:
            n1.assert_start_raises_init_error(extra_args=extra_args + self.pin, expected_msg=f"Error: {error}", match=ErrorMatch.PARTIAL_REGEX)
        assert self.snapshot_dir().exists()

    def test_assumeutxodata_off_regtest(self):
        self.log.info("-assumeutxodata is regtest-only")
        node = self.nodes[0]
        self.stop_node(0)
        conf_file = node.datadir_path / "dash.conf"
        existing_paths = set(node.datadir_path.iterdir())
        pin = f"{SNAPSHOT_BASE_HEIGHT}:{'1' * 64}:{'2' * 64}:300:{'3' * 64}"
        for chain_name in ["", "testnet3", "devnet"]:
            write_config(conf_file, n=0, chain=chain_name, extra_config=f"assumeutxodata={pin}\n")
            node.assert_start_raises_init_error(expected_msg="Error: -assumeutxodata is only available on regtest.")
        write_config(conf_file, n=0, chain="regtest")
        # Remove the chain dirs the failed starts created, as feature_config_args.py does.
        for path in set(node.datadir_path.iterdir()) - existing_paths:
            if path.is_dir():
                shutil.rmtree(path)
        self.start_node(0)


if __name__ == '__main__':
    AssumeutxoRefusalsTest().main()
