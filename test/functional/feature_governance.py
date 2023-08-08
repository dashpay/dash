#!/usr/bin/env python3
# Copyright (c) 2018-2020 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests around dash governance."""

import json
import time

from test_framework.messages import uint256_to_string
from test_framework.test_framework import DashTestFramework

class DashGovernanceTest (DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(6, 5)

    def prepare_object(self, object_type, parent_hash, creation_time, revision, name, amount, payment_address):
        proposal_rev = revision
        proposal_time = int(creation_time)
        proposal_template = {
            "type": object_type,
            "name": name,
            "start_epoch": proposal_time,
            "end_epoch": proposal_time + 24 * 60 * 60,
            "payment_amount": amount,
            "payment_address": payment_address,
            "url": "https://dash.org"
        }
        proposal_hex = ''.join(format(x, '02x') for x in json.dumps(proposal_template).encode())
        collateral_hash = self.nodes[0].gobject("prepare", parent_hash, proposal_rev, proposal_time, proposal_hex)
        return {
            "parentHash": parent_hash,
            "collateralHash": collateral_hash,
            "createdAt": proposal_time,
            "revision": proposal_rev,
            "hex": proposal_hex,
            "data": proposal_template,
        }

    def run_test(self):
        llmq_type=103
        llmq_type_name="llmq_test_dip0024"

        self.activate_dip8()

        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.nodes[0].sporkupdate("SPORK_9_SUPERBLOCKS_ENABLED", 0)
        self.wait_for_sporks_same()

        payout_address = self.nodes[0].getnewaddress()

        self.activate_v20(expected_activation_height=1440)
        self.log.info("Activated v20 at height:" + str(self.nodes[0].getblockcount()))

        #At this point, we need to move forward 3 cycles (3 x 24 blocks) so the first 3 quarters can be created (without DKG sessions)
        #self.log.info("Start at H height:" + str(self.nodes[0].getblockcount()))
        self.move_to_next_cycle()
        self.log.info("Cycle H height:" + str(self.nodes[0].getblockcount()))
        self.move_to_next_cycle()
        self.log.info("Cycle H+C height:" + str(self.nodes[0].getblockcount()))
        self.move_to_next_cycle()
        self.log.info("Cycle H+2C height:" + str(self.nodes[0].getblockcount()))

        (quorum_info_0_0, quorum_info_0_1) = self.mine_cycle_quorum(llmq_type_name=llmq_type_name, llmq_type=llmq_type)

        # At this point, we want to wait for CLs just before the self.mine_cycle_quorum to diversify the CLs in CbTx.
        # Although because here a new quorum cycle is starting, and we don't want to mine them now, mine 8 blocks (to skip all DKG phases)
        nodes = [self.nodes[0]] + [mn.node for mn in self.mninfo.copy()]
        self.nodes[0].generate(8)
        self.sync_blocks(nodes)
        self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash())

        proposal_time = self.mocktime
        object_type = 1  # GOVERNANCE PROPOSAL

        map_vote_outcomes = {
            0: "none",
            1: "yes",
            2: "no",
            3: "abstain"
        }
        map_vote_signals = {
            0: "none",
            1: "funding",
            2: "valid",
            3: "delete",
            4: "endorsed"
        }

        p0_collateral_prepare = self.prepare_object(object_type, uint256_to_string(0), proposal_time, 1, "Proposal_1", 1, payout_address)
        p1_collateral_prepare = self.prepare_object(object_type, uint256_to_string(0), proposal_time, 1, "Proposal_2", 3, payout_address)

        self.wait_for_instantlock(p0_collateral_prepare["collateralHash"], self.nodes[0])
        self.wait_for_instantlock(p1_collateral_prepare["collateralHash"], self.nodes[0])
        self.nodes[0].generate(6)
        self.sync_blocks()

        self.log.info("#### list-prepared:"+str(self.nodes[0].gobject("list-prepared")))

        p0_hash = self.nodes[0].gobject("submit", "0", 1, proposal_time, p0_collateral_prepare["hex"], p0_collateral_prepare["collateralHash"])
        p1_hash = self.nodes[0].gobject("submit", "0", 1, proposal_time, p1_collateral_prepare["hex"], p1_collateral_prepare["collateralHash"])

        self.nodes[0].gobject("vote-many", p0_hash, map_vote_signals[1], map_vote_outcomes[1])
        self.nodes[0].gobject("vote-many", p1_hash, map_vote_signals[1], map_vote_outcomes[1])

        for i in range(4):
            time.sleep(1)
            self.nodes[0].generate(1)
            self.sync_blocks(nodes)

        self.log.info("#### list:"+str(self.nodes[0].gobject("list")))
        self.log.info("#### getgovernanceinfo:"+str(self.nodes[0].getgovernanceinfo()))

        return


if __name__ == '__main__':
    DashGovernanceTest().main()
