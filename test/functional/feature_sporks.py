#!/usr/bin/env python3
# Copyright (c) 2018-2024 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.test_framework import BitcoinTestFramework

'''
'''

class SporkTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.disable_mocktime = True
        self.extra_args = [["-sporkkey=cP4EKFyJsHT39LDqgdcB43Y3YXjNyjb5Fuas1GQSeAtjnZWmZEQK"], [], []]

    def setup_network(self):
        self.setup_nodes()
        # connect only 2 first nodes at start
        self.connect_nodes(0, 1)

    def get_test_spork_value(self, node):
        # use InstantSend spork for tests
        return node.spork()['SPORK_2_INSTANTSEND_ENABLED']

    def set_test_spork_value(self, node, value):
        # use InstantSend spork for tests
        node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", value)

    def run_test(self):
        spork_default_value = self.get_test_spork_value(self.nodes[0])
        # check test spork default state matches on all nodes
        assert self.get_test_spork_value(self.nodes[1]) == spork_default_value
        assert self.get_test_spork_value(self.nodes[2]) == spork_default_value

        # check spork propagation for connected nodes
        spork_new_value = spork_default_value + 1
        self.set_test_spork_value(self.nodes[0], spork_new_value)
        self.wait_until(lambda: self.get_test_spork_value(self.nodes[1]) == spork_new_value, timeout=10)

        # restart nodes to check spork persistence
        self.stop_node(0)
        self.stop_node(1)
        self.start_node(0)
        self.start_node(1)
        assert self.get_test_spork_value(self.nodes[0]) == spork_new_value
        assert self.get_test_spork_value(self.nodes[1]) == spork_new_value

        # Generate one block to kick off masternode sync, which also starts sporks syncing for node2
        self.generate(self.nodes[1], 1, sync_fun=self.no_op)

        # connect new node and check spork propagation after restoring from cache
        self.connect_nodes(1, 2)
        self.wait_until(lambda: self.get_test_spork_value(self.nodes[2]) == spork_new_value, timeout=10)

if __name__ == '__main__':
    SporkTest().main()
