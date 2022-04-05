#!/usr/bin/env python3
# Copyright (c) 2015-2021 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_rotation.py

Checks LLMQs Quorum Rotation

'''

import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import connect_nodes, isolate_node, reconnect_isolated_node, sync_blocks, assert_equal, \
    assert_greater_than_or_equal, wait_until


def intersection(lst1, lst2):
    lst3 = [value for value in lst1 if value in lst2]
    return lst3


class LLMQQuorumRotationTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(21, 20, fast_dip3_enforcement=True)
        self.set_dash_llmq_test_params(4, 4)
        self.set_dash_dip24_activation(260)

    def run_test(self):

        # Connect all nodes to node1 so that we always have the whole network connected
        # Otherwise only masternode connections will be established between nodes, which won't propagate TXs/blocks
        # Usually node0 is the one that does this, but in this test we isolate it multiple times

        for i in range(len(self.nodes)):
            if i != 1:
                connect_nodes(self.nodes[i], 0)

        self.activate_dip8()

        self.nodes[0].spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        self.activate_dip24()
        self.log.info("Activated DIP24 at height:" + str(self.nodes[0].getblockcount()))

        cycle_length = 24

        #At this point, we need to move forward 3 cycles (3 x 24 blocks) so the first 3 quarters can be created (without DKG sessions)
        #self.log.info("Start at H height:" + str(self.nodes[0].getblockcount()))
        self.move_to_next_cycle(cycle_length)
        self.log.info("Cycle H height:" + str(self.nodes[0].getblockcount()))
        self.move_to_next_cycle(cycle_length)
        self.log.info("Cycle H+C height:" + str(self.nodes[0].getblockcount()))
        self.move_to_next_cycle(cycle_length)
        self.log.info("Cycle H+2C height:" + str(self.nodes[0].getblockcount()))

        self.mine_quorum()
        #self.move_to_next_cycle(cycle_length)
        #quorum_members_0 = self.extract_quorum_members()
        #self.log.info("Quorum #0 members: " + str(quorum_members_0))
        #self.log.info("QUORUMS:" + str(self.nodes[0].quorum("list", 1)))
        '''
        for i in range(26):
            time.sleep(2)
            self.bump_mocktime(1, nodes=nodes)
            self.nodes[0].generate(1)
            sync_blocks(nodes)
            self.log.info("--height:" + str(self.nodes[0].getblockcount()))
        '''
        #self.move_to_next_cycle(cycle_length)
        #self.move_to_next_cycle()
        #self.log.info("Normally first Quorum at height:" + str(self.nodes[0].getblockcount()))
        #self.mine_quorum()
        #quorum_members_0 = self.extract_quorum_members()
        #self.log.info("Quorum #0 members: " + str(quorum_members_0))
        #self.move_to_next_cycle(cycle_length)
        #self.log.info("Cycle H+3C height:" + str(self.nodes[0].getblockcount()))
        #self.move_to_next_cycle(cycle_length)
        #self.log.info("Cycle H+4C height:" + str(self.nodes[0].getblockcount()))

        #self.log.info("QUORUMS:" + str(self.nodes[0].quorum("list", 1)))
        #self.move_to_next_cycle()
        #self.log.info("End at height:" + str(self.nodes[0].getblockcount()))

        #check for timeout
        '''
        self.mine_quorum()
        quorum_members_0 = self.extract_quorum_members()
        self.log.info("Quorum #0 members: " + str(quorum_members_0))

        self.mine_quorum()
        quorum_members_1 = self.extract_quorum_members()
        self.log.info("Quorum #1 members: " + str(quorum_members_1))

        self.mine_quorum()
        quorum_members_2 = self.extract_quorum_members()
        self.log.info("Quorum #2 members: " + str(quorum_members_2))

        self.mine_quorum()
        quorum_members_3 = self.extract_quorum_members()
        self.log.info("Quorum #3 members: " + str(quorum_members_3))

        quorum_common_members_0_1 = intersection(quorum_members_0, quorum_members_1)
        quorum_common_members_0_2 = intersection(quorum_members_0, quorum_members_2)
        quorum_common_members_0_3 = intersection(quorum_members_0, quorum_members_3)

        quorum_common_members_1_2 = intersection(quorum_members_1, quorum_members_2)
        quorum_common_members_1_3 = intersection(quorum_members_1, quorum_members_3)

        quorum_common_members_2_3 = intersection(quorum_members_2, quorum_members_3)

        #We test with greater_than_or_equal instead of only equal because with few MNs available, sometimes MNs are re-selected
        assert_greater_than_or_equal (len(quorum_common_members_0_1), 3)
        assert_greater_than_or_equal (len(quorum_common_members_0_2), 2)
        assert_greater_than_or_equal (len(quorum_common_members_0_3), 1)

        assert_greater_than_or_equal (len(quorum_common_members_1_2), 3)
        assert_greater_than_or_equal (len(quorum_common_members_1_3), 2)

        assert_equal (len(quorum_common_members_2_3), 3)
        '''
    def move_to_next_cycle(self, cycle_length):
        mninfos_online = self.mninfo.copy()
        nodes = [self.nodes[0]] + [mn.node for mn in mninfos_online]

        # move forward to next DKG
        skip_count = cycle_length - (self.nodes[0].getblockcount() % cycle_length)
        if skip_count != 0:
            self.bump_mocktime(1, nodes=nodes)
            self.nodes[0].generate(skip_count)
        sync_blocks(nodes)

    def extract_quorum_members(self):
        quorum = self.nodes[0].quorum("list", 1)["llmq_test"][0]
        quorum_info = self.nodes[0].quorum("info", 100, quorum)
        return [d['proTxHash'] for d in quorum_info["members"]]

if __name__ == '__main__':
    LLMQQuorumRotationTest().main()
