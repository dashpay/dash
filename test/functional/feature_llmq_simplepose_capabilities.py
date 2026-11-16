#!/usr/bin/env python3
# Copyright (c) 2015-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_simplepose_capabilities.py

Checks simple PoSe system based on LLMQ commitments: banning of masternodes
that run an outdated protocol or lack a required service.

'''

from test_framework.masternodes import check_banned, check_punished
from test_framework.messages import NODE_COMPACT_FILTERS
from test_framework.p2p import P2P_SERVICES, P2PInterface
from test_framework.simplepose import SimplePoSeMixin
from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.util import assert_equal, force_finish_mnsync

# See version.h
MIN_MASTERNODE_PROTO_VERSION = 70242


class MasternodePeer(P2PInterface):
    """Connects to a masternode the way another masternode does, advertising the given relay flag"""
    def __init__(self, relay):
        super().__init__()
        self.version_relay = relay

    def peer_connect_send_version(self, services):
        super().peer_connect_send_version(services)
        self.on_connection_send_msg.relay = self.version_relay
        self.on_connection_send_msg.other_masternode = True


class LLMQSimplePoSeCapabilitiesTest(SimplePoSeMixin, DashTestFramework):
    def set_test_params(self):
        self.set_simplepose_test_params()

    def run_test(self):
        self.set_pose_sporks()

        # These scenarios need SPORK_21 on, an existing quorum and all masternodes healthy
        self.nodes[0].sporkupdate("SPORK_21_QUORUM_ALL_CONNECTED", 0)
        self.wait_for_sporks_same()
        self.reset_probe_timeouts()
        self.mine_quorum()

        if not self.options.disable_spork23:
            self.test_banning(self.force_old_mn_proto, 3)
        else:
            # With PoSe off there should be no punishing for outdated nodes
            self.test_no_banning(self.force_old_mn_proto, 3)

        self.repair_masternodes(True)
        self.reset_probe_timeouts()

        for relay, services, reason in [(0, P2P_SERVICES | NODE_COMPACT_FILTERS, "does not relay transactions"),
                                        (1, P2P_SERVICES, "does not serve compact block filters")]:
            self.test_no_service(relay, services, reason, banned=not self.options.disable_spork23)
            self.repair_masternodes(True)
            self.reset_probe_timeouts()

    def force_old_mn_proto(self, mn: MasternodeInfo):
        self.stop_node(mn.nodeIdx)
        self.start_masternode(mn, [f"-pushversion={MIN_MASTERNODE_PROTO_VERSION - 1}"])
        self.connect_nodes(mn.nodeIdx, 0)
        self.reset_probe_timeouts()
        return False, True

    def test_no_service(self, relay, services, reason, banned):
        # A member is judged by what our masternode connections to it advertise in their version message. A stock
        # masternode always advertises both services, so a P2P peer stands in for the connections to one member.
        mn = self.mninfo[0]
        node = mn.get_node(self)
        others = [m for m in self.mninfo if m is not mn]
        expected_complaints = len(others) if banned else 0
        for i in range(2):
            self.log.info(f"Testing PoSe {'banning' if banned else 'no banning'} of a masternode that {reason} {i + 1}/2")
            self.reset_probe_timeouts()
            with others[0].get_node(self).assert_debug_log([reason] if banned else [], unexpected_msgs=[] if banned else [reason]):
                self.mine_quorum_no_service(mn, others, relay, services, expected_complaints)
            for other in others:
                other.get_node(self).disconnect_p2ps()
            node.setnetworkactive(True)
            force_finish_mnsync(node)
            self.connect_nodes(mn.nodeIdx, 0)
            self.sync_blocks()
            if check_banned(self.nodes[0], mn):
                break
        assert_equal(check_banned(self.nodes[0], mn), banned)
        if not banned:
            assert not check_punished(self.nodes[0], mn)

    def mine_quorum_no_service(self, mn, others, relay, services, expected_complaints):
        # Like mine_quorum, except that mn goes offline once every member of every type holds its contribution and
        # a peer stands in for it on each other member with a masternode connection that lacks the service, so that
        # the complain phase judges mn by that connection alone.
        self.log.info(f"Mining quorum with a stand-in for {mn.proTxHash}: expected_complaints={expected_complaints}")
        nodes = [self.nodes[0]] + [m.get_node(self) for m in others]
        spork23_active = self.nodes[0].spork('show')['SPORK_23_QUORUM_POSE'] <= 1

        # move forward to next DKG
        skip_count = 24 - (self.nodes[0].getblockcount() % 24)
        self.bump_mocktime(1)
        self.generate(self.nodes[0], skip_count)

        q = self.nodes[0].getbestblockhash()
        self.log.info("Expected quorum_hash:"+str(q))
        self.log.info("Waiting for phase 1 (init)")
        self.wait_for_quorum_phase(q, 1, len(self.mninfo), None, 0, self.mninfo, all_types=True)
        self.wait_for_quorum_connections(q, self.llmq_size - 1, self.mninfo, wait_proc=lambda: self.bump_mocktime(1), all_types=True)
        if spork23_active:
            self.wait_for_masternode_probes(q, others, wait_proc=lambda: self.bump_mocktime(1))

        self.move_blocks(self.nodes, 2)

        self.log.info("Waiting for phase 2 (contribute)")
        self.wait_for_quorum_phase(q, 2, len(self.mninfo), "receivedContributions", self.llmq_size, self.mninfo, all_types=True)
        self.wait_for_offset_quorum_phase(q, 2, self.mninfo)

        self.log.info("Replacing the connections to the masternode with stand-ins")
        mn.get_node(self).setnetworkactive(False)
        for other in others:
            other_node = other.get_node(self)
            self.wait_until(lambda: all(p.get("verified_proregtx_hash") != mn.proTxHash for p in other_node.getpeerinfo()))
            peer = other_node.add_p2p_connection(MasternodePeer(relay), services=services)
            peer_ids = [p["id"] for p in other_node.getpeerinfo() if p["subver"] == peer.strSubVer]
            assert_equal(len(peer_ids), 1)
            assert other_node.mnauth(peer_ids[0], mn.proTxHash, mn.pubKeyOperator)

        self.move_blocks(nodes, 2)

        self.log.info("Waiting for phase 3 (complain)")
        self.wait_for_quorum_phase(q, 3, len(others), "receivedComplaints", expected_complaints, others)
        self.wait_for_offset_quorum_phase(q, 3, others, messages=False)

        self.move_blocks(nodes, 2)

        self.log.info("Waiting for phase 4 (justify)")
        self.wait_for_quorum_phase(q, 4, len(others), "receivedJustifications", 0, others)
        self.wait_for_offset_quorum_phase(q, 4, others, messages=False)

        self.move_blocks(nodes, 2)

        self.log.info("Waiting for phase 5 (commit)")
        self.wait_for_quorum_phase(q, 5, len(others), "receivedPrematureCommitments", len(others), others)
        self.wait_for_offset_quorum_phase(q, 5, others, messages=False)

        self.move_blocks(nodes, 2)

        self.log.info("Waiting for phase 6 (mining)")
        self.wait_for_quorum_phase(q, 6, len(others), None, 0, others)
        self.wait_for_offset_quorum_phase(q, 6, others, messages=False)

        self.log.info("Waiting final commitment")
        self.wait_for_quorum_commitment(q, others)

        self.log.info("Waiting final commitments on mining node")
        self.wait_for_quorum_commitments_on_miner(q, others)

        self.log.info("Mining final commitment")
        self.bump_mocktime(1)
        self.nodes[0].getblocktemplate() # this calls CreateNewBlock
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks(nodes))

        self.log.info("Waiting for quorum to appear in the list")
        self.wait_for_quorum_list(q, nodes)
        assert_equal(q, self.nodes[0].quorum("list", 1)["llmq_test"][0])

        self.wait_for_offset_quorum_commitments(q, others)

        # Mine 8 (SIGN_HEIGHT_OFFSET) more blocks to make sure that the new quorum gets eligible for signing sessions
        self.generate(self.nodes[0], 8, sync_fun=lambda: self.sync_blocks(nodes))

        for m in others:
            assert not check_punished(self.nodes[0], m)
            assert not check_banned(self.nodes[0], m)


if __name__ == '__main__':
    LLMQSimplePoSeCapabilitiesTest().main()
