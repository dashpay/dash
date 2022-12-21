#!/usr/bin/env python3
# Copyright (c) 2015-2022 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_rotation.py

Checks LLMQs Quorum Rotation

'''
from test_framework.test_framework import DashTestFramework
from test_framework.util import (
    assert_equal
)


def intersection(lst1, lst2):
    lst3 = [value for value in lst1 if value in lst2]
    return lst3


def extract_quorum_members(quorum_info):
    return [d['proTxHash'] for d in quorum_info["members"]]


class LLMQHPMNTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(9, 8, fast_dip3_enforcement=True, hpmn_count=4)
        self.set_dash_llmq_test_params(4, 4)

    def run_test(self):
        llmq_type = 106
        llmq_type_name = "llmq_test_platform"

        # Connect all nodes to node1 so that we always have the whole network connected
        # Otherwise only masternode connections will be established between nodes, which won't propagate TXs/blocks
        # Usually node0 is the one that does this, but in this test we isolate it multiple times

        for i in range(len(self.nodes)):
            if i != 1:
                self.connect_nodes(i, 0)

        self.activate_dip8()

        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        quorum_0_hash = self.mine_quorum(llmq_type_name, llmq_type)
        self.test_quorum_members_are_high_performance(llmq_type, quorum_0_hash)

        quorum_1_hash = self.mine_quorum(llmq_type_name, llmq_type)
        self.test_quorum_members_are_high_performance(llmq_type, quorum_1_hash)

        quorum_2_hash = self.mine_quorum(llmq_type_name, llmq_type)
        self.test_quorum_members_are_high_performance(llmq_type, quorum_2_hash)

        return

    def test_quorum_members_are_high_performance(self, llmq_type, quorum_hash):
        quorum_info = self.nodes[0].quorum("info", llmq_type, quorum_hash)
        quorum_members = extract_quorum_members(quorum_info)
        mninfos_online = self.mninfo.copy()
        for qm in quorum_members:
            found = False
            for mn in mninfos_online:
                if mn.proTxHash == qm:
                    assert_equal(mn.hpmn, True)
                    found = True
                    break
            assert_equal(found, True)


if __name__ == '__main__':
    LLMQHPMNTest().main()
