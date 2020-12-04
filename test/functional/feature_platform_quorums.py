#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_bip9_status

'''
feature_platform_quorums.py

Checks block reward reallocation correctness

'''


class PlatformQuorumActivationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        assert_equal(get_bip9_status(self.nodes[0], 'v17')['status'], 'locked_in')
        ql = self.nodes[0].quorum("list")
        assert_equal(len(ql), 1)
        assert("llmq_test_v17" not in ql)
        self.nodes[0].generate(99)
        assert_equal(get_bip9_status(self.nodes[0], 'v17')['status'], 'active')
        self.nodes[0].generate(1)
        ql = self.nodes[0].quorum("list")
        assert_equal(len(ql), 2)
        assert("llmq_test_v17" in ql)


if __name__ == '__main__':
    PlatformQuorumActivationTest().main()
