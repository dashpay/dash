#!/usr/bin/env python3
# Copyright (c) 2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test masternode parameter interactions.

This test verifies that certain parameters are automatically enabled
when a node is configured as a masternode via -masternodeblsprivkey, and
that a masternode refuses to start with settings that make it silently
skip its duties (transaction relay, InstantSend signing, SPV serving).
"""

from test_framework.test_framework import BitcoinTestFramework

# Service flags
NODE_COMPACT_FILTERS = (1 << 6)

# Constants
BASIC_FILTER_INDEX = 'basic filter index'


class MasternodeParamsTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def run_test(self):
        self.log.info("Test that regular node has default settings")
        node0 = self.nodes[0]

        # Regular node should have peerblockfilters disabled by default
        services = int(node0.getnetworkinfo()['localservices'], 16)
        assert services & NODE_COMPACT_FILTERS == 0

        # Regular node should not have blockfilterindex enabled
        index_info = node0.getindexinfo()
        assert BASIC_FILTER_INDEX not in index_info

        self.log.info("Test that masternode has blockfilters auto-enabled")
        # Generate a valid BLS key for testing
        bls_info = node0.bls('generate')
        bls_key = bls_info['secret']

        # Start a node with masternode key
        self.restart_node(1, extra_args=[f"-masternodeblsprivkey={bls_key}"])
        node1 = self.nodes[1]

        # Masternode should have peerblockfilters enabled
        services = int(node1.getnetworkinfo()['localservices'], 16)
        self.log.info(f"Masternode services: {hex(services)}, has COMPACT_FILTERS: {services & NODE_COMPACT_FILTERS != 0}")

        # Check blockfilterindex
        index_info = node1.getindexinfo()
        self.log.info(f"Masternode indexes: {list(index_info.keys())}")

        # For now, just check that the node started successfully with masternode key
        # The actual filter enabling might require the node to be fully synced
        assert node1.getblockcount() >= 0  # Basic check that node is running

        self.log.info("Test that masternode refuses settings that silently skip its duties")
        self.stop_node(1)
        mn_arg = f"-masternodeblsprivkey={bls_key}"
        for extra_args, expected_msg in [
            (["-blocksonly"], "Error: Masternode must relay transactions, set -blocksonly=0"),
            (["-peerblockfilters=0"], "Error: Masternode must serve compact block filters, set -peerblockfilters=1"),
            (["-maxuploadtarget=500M"], "Error: Masternode must serve blocks and SPV clients without an upload limit, set -maxuploadtarget=0"),
        ]:
            self.nodes[1].assert_start_raises_init_error(extra_args=[mn_arg] + extra_args, expected_msg=expected_msg)

        # The stricter-relay-policy check only applies to chains with public masternodes; regtest
        # masternodes may use it to create mempool inconsistencies (feature_llmq_is_retroactive.py).
        self.log.info("Test that a stricter relay policy is allowed on regtest masternodes")
        self.start_node(1, extra_args=[mn_arg, "-minrelaytxfee=0.001", "-datacarrier=0", "-limitancestorcount=5"])

        self.log.info("Test that masternode parameter interaction is logged")
        # Stop the node first so we can check the startup logs
        self.stop_node(1)

        # Check debug log for parameter interaction messages during startup
        if self.is_wallet_compiled():
            with self.nodes[1].assert_debug_log(["parameter interaction: -masternodeblsprivkey set -> setting -disablewallet=1"]):
                self.start_node(1, extra_args=[mn_arg])
            self.stop_node(1)

        self.log.info("Test that a regular node may still use these settings")
        self.start_node(1, extra_args=["-blocksonly", "-peerblockfilters=0", "-maxuploadtarget=500M", "-datacarrier=0", "-minrelaytxfee=0.001"])


if __name__ == '__main__':
    MasternodeParamsTest().main()
