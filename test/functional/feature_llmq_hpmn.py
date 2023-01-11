#!/usr/bin/env python3
# Copyright (c) 2015-2022 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_rotation.py

Checks LLMQs Quorum Rotation

'''
from collections import defaultdict
from decimal import Decimal
import json
import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import (
    assert_equal, force_finish_mnsync, p2p_port
)


def intersection(lst1, lst2):
    lst3 = [value for value in lst1 if value in lst2]
    return lst3


def extract_quorum_members(quorum_info):
    return [d['proTxHash'] for d in quorum_info["members"]]

class HPMN(object):
    pass

class LLMQHPMNTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(9, 4, fast_dip3_enforcement=True, hpmn_count=4)
        self.set_dash_llmq_test_params(4, 4)

        #self.supports_cli = False

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

        self.activate_v19(expected_activation_height=900)
        self.log.info("Activated v19 at height:" + str(self.nodes[0].getblockcount()))

        self.log.info("Test HPMN payments")
        self.test_hpmmn_payements(window_analysis=48)

        self.log.info("Test llmq_platform are formed only with HPMN")
        quorum_0_hash = self.mine_quorum(llmq_type_name, llmq_type)
        self.test_quorum_members_are_high_performance(llmq_type, quorum_0_hash)

        quorum_1_hash = self.mine_quorum(llmq_type_name, llmq_type)
        self.test_quorum_members_are_high_performance(llmq_type, quorum_1_hash)

        quorum_2_hash = self.mine_quorum(llmq_type_name, llmq_type)
        self.test_quorum_members_are_high_performance(llmq_type, quorum_2_hash)



        return

    def prepare_hpmn(self, node, idx, alias):
        hpmn = HPMN()
        hpmn.idx = idx
        hpmn.alias = alias
        hpmn.p2p_port = p2p_port(hpmn.idx)

        address = node.getnewaddress()
        blsKey = node.bls('generate')
        hpmn.fundsAddr = address
        hpmn.ownerAddr = address
        hpmn.operatorAddr = blsKey['public']
        hpmn.votingAddr = address
        hpmn.blsMnkey = blsKey['secret']

        return hpmn

    def register_fund_mn(self, node, mn):
        node.sendtoaddress(mn.fundsAddr, 1000.001)
        mn.collateral_address = node.getnewaddress()
        mn.rewards_address = node.getnewaddress()

        mn.protx_hash = node.protx('register_fund', mn.collateral_address, '127.0.0.1:%d' % mn.p2p_port, mn.ownerAddr, mn.operatorAddr, mn.votingAddr, 0, mn.rewards_address, mn.fundsAddr)
        mn.collateral_txid = mn.protx_hash
        mn.collateral_vout = None

        rawtx = node.getrawtransaction(mn.collateral_txid, 1)
        for txout in rawtx['vout']:
            if txout['value'] == Decimal(1000):
                mn.collateral_vout = txout['n']
                break
        assert mn.collateral_vout is not None

        self.log.info(">"+str(node.getrawtransaction(mn.protx_hash, 1)))

    def start_mn(self, mn):
        self.log.info("len(self.nodes) = " + str(len(self.nodes)) + " mn.idx = " + str(mn.idx))
        if len(self.nodes) <= mn.idx:
            self.log.info("len(self.nodes) = " + str(len(self.nodes)) + " mn.idx = " + str(mn.idx))
            self.add_nodes(mn.idx - len(self.nodes) + 1)
        #    assert len(self.nodes) == mn.idx + 1
        #self.start_node(mn.idx, extra_args = self.extra_args + ['-masternodeblsprivkey=%s' % mn.blsMnkey])
        #force_finish_mnsync(self.nodes[mn.idx])
        #mn.node = self.nodes[mn.idx]
        #self.connect_nodes(mn.idx, 0)
        #self.sync_all()

    def test_hpmmn_payements(self, window_analysis):
        mn_payees = list()

        for i in range(0, window_analysis):
            payee_info = self.get_mn_payee_for_block(self.nodes[0].getbestblockhash())
            mn_payees.append(payee_info)

            self.nodes[0].generate(1)
            self.sync_blocks()

        verified_hpmn = None
        for i in range(len(mn_payees)):
            # Start checking from the first payee different from the first element of the window analysis
            if i > 0 and mn_payees[i] != mn_payees[0]:
                # Check only HPMN
                if mn_payees[i].hpmn:
                    # Skip already checked payee
                    if mn_payees[i].proTxHash == verified_hpmn:
                        continue
                    # Verify that current HPMN is payed for 4 blocks in a row
                    for j in range(1, 4):
                        # Avoid overflow check
                        if (i + j) < len(mn_payees):
                            assert_equal(mn_payees[i].proTxHash, mn_payees[i+j].proTxHash)
                    verified_hpmn = mn_payees[i].proTxHash

    def get_mn_payee_for_block(self, block_hash):

        mn_payee_info = self.nodes[0].masternode("payments", block_hash)[0]
        mn_payee_protx = mn_payee_info['masternodes'][0]['proTxHash']

        mninfos_online = self.mninfo.copy()
        for mn_info in mninfos_online:
            if mn_info.proTxHash == mn_payee_protx:
                return mn_info
        return None

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
