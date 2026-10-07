#!/usr/bin/env python3
# Copyright (c) 2015-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_connections.py

Checks intra quorum connections

'''

import time

from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.util import assert_greater_than_or_equal, force_finish_mnsync

class LLMQConnections(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_dash_test_params(11, 10)
        self.set_dash_llmq_test_params(5, 3)
        # Probes should age after this many seconds.
        # NOTE: mine_quorum() can bump mocktime quite often internally so make sure this number is high enough.
        self.MAX_AGE = int(120 * self.options.timeout_factor)

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        q = self.mine_quorum()

        self.log.info("checking for old intra quorum connections")
        total_count = 0
        for mn in self.get_quorum_masternodes(q):
            count = self.get_mn_connection_count(mn.get_node(self))
            total_count += count
            assert_greater_than_or_equal(count, 2)
        assert total_count < 25

        self.check_reconnects(2)

        self.log.info("Activating SPORK_23_QUORUM_POSE")
        self.nodes[0].sporkupdate("SPORK_23_QUORUM_POSE", 0)
        self.wait_for_sporks_same()

        self.log.info("mining one block and waiting for all members to connect to each other")
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        for mn in self.get_quorum_masternodes(q):
            self.wait_for_mnauth(mn.get_node(self), 4)

        self.log.info("mine a new quorum and verify that all members connect to each other")
        q = self.mine_quorum()

        self.log.info("checking that all MNs got probed")
        for mn in self.get_quorum_masternodes(q):
            self.wait_until(lambda: self.get_mn_probe_count(mn.get_node(self), q, False) == 4)

        self.log.info("checking that probes age")
        self.bump_mocktime(self.MAX_AGE)
        for mn in self.get_quorum_masternodes(q):
            self.wait_until(lambda: self.get_mn_probe_count(mn.get_node(self), q, False) == 0)

        self.log.info("mine a new quorum and re-check probes")
        q = self.mine_quorum()
        for mn in self.get_quorum_masternodes(q):
            self.wait_until(lambda: self.get_mn_probe_count(mn.get_node(self), q, True) == 4)

        self.log.info("Activating SPORK_21_QUORUM_ALL_CONNECTED")
        self.nodes[0].sporkupdate("SPORK_21_QUORUM_ALL_CONNECTED", 0)
        self.wait_for_sporks_same()

        self.check_reconnects(4)

        self.nodes[0].sporkupdate("SPORK_23_QUORUM_POSE", 4070908800)
        self.wait_for_sporks_same()

        self.mine_cycle_quorum()

        # Since we IS quorums are mined only using dip24 (rotation) we need to enable rotation, and continue tests on llmq_test_dip0024 for connections.

        self.log.info("check that old masternode connections are dropped")
        removed = False
        for mn in self.mninfo: # type: MasternodeInfo
            if len(mn.get_node(self).quorum("memberof", mn.proTxHash)) > 0:
                try:
                    with mn.get_node(self).assert_debug_log(['removing masternodes quorum connections']):
                        with mn.get_node(self).assert_debug_log(['keeping mn quorum connections']):
                            self.mine_cycle_quorum()
                            mn.get_node(self).mockscheduler(60) # we check for old connections via the scheduler every 60 seconds
                    removed = True
                except Exception:
                    pass # it's ok to not remove connections sometimes
            if removed:
                break
        assert removed # no way we removed none

        self.log.info("check that inter-quorum masternode connections are added")
        added = False
        for mn in self.mninfo: # type: MasternodeInfo
            if len(mn.get_node(self).quorum("memberof", mn.proTxHash)) > 0:
                try:
                    with mn.get_node(self).assert_debug_log(['adding mn inter-quorum connections']):
                        self.mine_cycle_quorum()
                    added = True
                except Exception:
                    pass # it's ok to not add connections sometimes
            if added:
                break
        assert added # no way we added none

        self.test_instantsend_after_restart()

    def test_instantsend_after_restart(self):
        self.log.info("Testing InstantSend works after full restart without new blocks")

        # node0 missed the EHF signal txs the quorum members created while it was disconnected from them
        for mn_info in self.mninfo:
            for txid in mn_info.get_node(self).getrawmempool():
                self.nodes[0].sendrawtransaction(mn_info.get_node(self).getrawtransaction(txid))

        # fund sender with confirmed coins
        sender = self.nodes[0]
        receiver = self.nodes[0]
        sender_addr = sender.getnewaddress()
        fund_id = self.nodes[0].sendtoaddress(sender_addr, 1)
        self.wait_for_instantlock(fund_id)
        tip = self.generate(self.nodes[0], 2)[-1]
        self.bump_mocktime(30)
        self.wait_for_chainlocked_block_all_nodes(tip)
        self.sync_blocks()
        assert sender.getbalance() >= 0.5

        receiver_addr = receiver.getnewaddress()

        # restart all nodes without mining new blocks
        self.log.info("Restarting all nodes")
        num_simple_nodes = self.num_nodes - self.mn_count
        self.stop_nodes()

        for i in range(num_simple_nodes):
            self.start_node(i)
        for mn_info in self.mninfo:
            self.start_masternode(mn_info)

        # reconnect: simple nodes to node 0, MNs to node 0 only.
        # Quorum connections between MNs must be re-established automatically
        # via InitializeCurrentBlockTip → EnsureQuorumConnections, NOT via
        # manual connect_nodes between MN pairs.
        for i in range(1, num_simple_nodes):
            self.connect_nodes(i, 0)
        for mn_info in self.mninfo:
            self.connect_nodes(mn_info.nodeIdx, 0)
        for i in range(num_simple_nodes):
            force_finish_mnsync(self.nodes[i])

        # bump past WAIT_FOR_ISLOCK_TIMEOUT so txFirstSeenTime loss doesn't
        # block chainlock signing for TXs mined before restart
        self.bump_mocktime(10 * 60 + 1)
        self.sync_blocks()

        # Verify that MNs formed quorum connections to other MNs after restart.
        # InitializeCurrentBlockTip → EnsureQuorumConnections must populate
        # masternodeQuorumNodes so ThreadOpenMasternodeConnections establishes
        # MN-to-MN links beyond the manual connections to node 0.
        self.log.info("Verifying MN-to-MN quorum connections formed after restart")
        for llmq_type, llmq_type_name in ((100, 'llmq_test'), (103, 'llmq_test_dip0024')):
            for q in self.nodes[0].quorum('list')[llmq_type_name]:
                members = self.get_quorum_masternodes(q, llmq_type)
                for mn_info in members:
                    others = {m.proTxHash for m in members if m is not mn_info}

                    def check_mn_peers(node=mn_info.get_node(self), others=others):
                        peers = [p['verified_proregtx_hash'] for p in node.getpeerinfo() if p.get('verified_proregtx_hash')]
                        # a member listed twice still has a duplicate connection on its way out, and a sig
                        # share queued to that one is lost
                        return others <= set(peers) and len(peers) == len(set(peers))
                    self.wait_until(check_mn_peers, timeout=30)

        # re-grab references after restart
        sender = self.nodes[0]
        receiver = self.nodes[0]

        # send a TX — needs IS lock from all restarted MNs, no new blocks mined
        is_id = sender.sendtoaddress(receiver_addr, 0.5)
        self.wait_for_instantlock(is_id)
        self.log.info("InstantSend lock succeeded after full restart")

        # clean up
        receiver.sendtoaddress(self.nodes[0].getnewaddress(), 0.5, "", "", True)
        self.bump_mocktime(30)
        self.sync_mempools()
        self.generate(self.nodes[0], 2)

    def check_reconnects(self, expected_connection_count):
        self.log.info("disable and re-enable networking on all masternodes")
        for mn in self.mninfo: # type: MasternodeInfo
            mn.get_node(self).setnetworkactive(False)
        for mn in self.mninfo: # type: MasternodeInfo
            self.wait_until(lambda: len(mn.get_node(self).getpeerinfo()) == 0)
        for mn in self.mninfo: # type: MasternodeInfo
            mn.get_node(self).setnetworkactive(True)
        self.bump_mocktime(60)

        self.log.info("verify that all masternodes re-connected")
        for q in self.nodes[0].quorum('list')['llmq_test']:
            for mn in self.get_quorum_masternodes(q):
                self.wait_for_mnauth(mn.get_node(self), expected_connection_count)

        # Also re-connect non-masternode connections
        for i in range(1, len(self.nodes)):
            self.connect_nodes(i, 0)
            self.nodes[i].ping()
        # wait for ping/pong so that we can be sure that spork propagation works
        time.sleep(1) # needed to make sure we don't check before the ping is actually sent (fPingQueued might be true but SendMessages still not called)
        for i in range(1, len(self.nodes)):
            self.wait_until(lambda: all('pingwait' not in peer for peer in self.nodes[i].getpeerinfo()))

    def get_mn_connection_count(self, node):
        peers = node.getpeerinfo()
        count = 0
        for p in peers:
            if 'verified_proregtx_hash' in p and p['verified_proregtx_hash'] != '':
                count += 1
        return count

    def get_mn_probe_count(self, node, q, check_peers):
        count = 0
        mnList = node.protx('list', 'registered', True)
        peerList = node.getpeerinfo()
        mnMap = {}
        peerMap = {}
        for mn in mnList:
            mnMap[mn['proTxHash']] = mn
        for p in peerList:
            if 'verified_proregtx_hash' in p and p['verified_proregtx_hash'] != '':
                peerMap[p['verified_proregtx_hash']] = p
        for mn in self.get_quorum_masternodes(q):
            pi = mnMap[mn.proTxHash]
            if pi['metaInfo']['lastOutboundSuccessElapsed'] < self.MAX_AGE:
                count += 1
            elif check_peers and mn.proTxHash in peerMap:
                count += 1
        return count


if __name__ == '__main__':
    LLMQConnections().main()
