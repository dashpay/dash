#!/usr/bin/env python3
# Copyright (c) 2020-2021 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
import time

from test_framework.messages import CTransaction, FromHex, hash256, ser_compact_size, ser_string
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_raises_rpc_error, satoshi_round, wait_until, connect_nodes, sync_blocks

'''
feature_llmq_is_migration.py

Test IS LLMQ migration with DIP24

'''

class LLMQISMigrationTest(DashTestFramework):
    def set_test_params(self):
        # -whitelist is needed to avoid the trickling logic on node0
        self.set_dash_test_params(16, 15, [["-whitelist=127.0.0.1"], [], [], [], [], [], [], [], [], [], [], [], [], [], [], []], fast_dip3_enforcement=True)
        self.set_dash_llmq_test_params(4, 4)

        for i in range(0, self.num_nodes):
            self.extra_args[i].append("-llmqinstantsenddip24=llmq_test_2")

    def get_request_id(self, tx_hex):
        tx = FromHex(CTransaction(), tx_hex)

        request_id_buf = ser_string(b"islock") + ser_compact_size(len(tx.vin))
        for txin in tx.vin:
            request_id_buf += txin.prevout.serialize()
        return hash256(request_id_buf)[::-1].hex()

    def run_test(self):

        for i in range(len(self.nodes)):
            if i != 1:
                connect_nodes(self.nodes[i], 0)

        self.activate_dip8()

        node = self.nodes[0]
        node.spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
        node.spork("SPORK_2_INSTANTSEND_ENABLED", 0)
        self.wait_for_sporks_same()

        self.mine_quorum()
        self.mine_quorum()

        txid1 = node.sendtoaddress(node.getnewaddress(), 1)
        self.wait_for_instantlock(txid1, node)

        request_id = self.get_request_id(self.nodes[0].getrawtransaction(txid1))
        wait_until(lambda: node.quorum("hasrecsig", 100, request_id, txid1))

        rec_sig = node.quorum("getrecsig", 100, request_id, txid1)['sig']
        assert node.verifyislock(request_id, txid1, rec_sig)

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

        (quorum_info_0_0, quorum_info_0_1) = self.mine_cycle_quorum("llmq_test_2", 103)

        txid2 = node.sendtoaddress(node.getnewaddress(), 1)
        i = node.getrawtransaction(txid2, True);
        self.wait_for_instantlock(txid2, node)

        request_id2 = self.get_request_id(self.nodes[0].getrawtransaction(txid2))
        wait_until(lambda: node.quorum("hasrecsig", 103, request_id2, txid2))

        rec_sig2 = node.quorum("getrecsig", 103, request_id2, txid2)['sig']
        assert node.verifyislock(request_id2, txid2, rec_sig2)

    def move_to_next_cycle(self, cycle_length):
        mninfos_online = self.mninfo.copy()
        nodes = [self.nodes[0]] + [mn.node for mn in mninfos_online]
        cur_block = self.nodes[0].getblockcount()

        # move forward to next DKG
        skip_count = cycle_length - (cur_block % cycle_length)
        if skip_count != 0:
            self.bump_mocktime(1, nodes=nodes)
            self.nodes[0].generate(skip_count)
        sync_blocks(nodes)
        time.sleep(1)
        self.log.info('Moved from block %d to %d' % (cur_block, self.nodes[0].getblockcount()))

if __name__ == '__main__':
    LLMQISMigrationTest().main()
