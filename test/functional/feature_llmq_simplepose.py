#!/usr/bin/env python3
# Copyright (c) 2015-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_simplepose.py

Checks simple PoSe system based on LLMQ commitments: banning of masternodes
that are unreachable. See feature_llmq_simplepose_capabilities.py for
masternodes that run an outdated protocol or lack a required service.

'''

from test_framework.masternodes import check_banned, check_punished
from test_framework.simplepose import SimplePoSeMixin
from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.util import assert_equal


class LLMQSimplePoSeTest(SimplePoSeMixin, DashTestFramework):
    def set_test_params(self):
        self.set_simplepose_test_params()

    def run_test(self):
        self.set_pose_sporks()

        self.test_cloned_contributions()

        if not self.options.disable_spork23:
            # Lets isolate MNs one by one and verify that punishment/banning happens
            self.test_banning(self.isolate_mn, 2)

            self.repair_masternodes(False)
        else:
            # The contribution-miss ban path (MarkBadMember -> PoSePunish) is not
            # gated on spork23 (spork23 only gates connection/proto-version checks
            # and probes), so it behaves identically with spork23 disabled and is
            # already covered by the spork23-enabled run of this test.
            self.log.info("Skipping contribution-miss banning, not affected by spork23")
            # Mine one quorum in normal conditions so that the sections below start
            # from the same state as in the spork23-enabled run: an existing quorum
            # and all masternodes healthy.
            self.reset_probe_timeouts()
            self.mine_quorum()

        self.nodes[0].sporkupdate("SPORK_21_QUORUM_ALL_CONNECTED", 0)
        self.wait_for_sporks_same()

        self.reset_probe_timeouts()

        if not self.options.disable_spork23:
            # Lets restart masternodes with closed ports and verify that they get banned even though they are connected to other MNs (via outbound connections)
            self.test_banning(self.close_mn_port)
        else:
            # With PoSe off there should be no punishing for non-reachable nodes
            self.test_no_banning(self.close_mn_port, 3)

        self.repair_masternodes(True)

    def test_cloned_contributions(self):
        self.log.info("Two members dealing identical contributions are both excluded and the quorum still forms")
        cloners = self.mninfo[:2]
        others = self.mninfo[2:]
        for mn in cloners:
            mn.get_node(self).quorum('dkgsimerror', 'contribution-clone', 100)
        qh = self.mine_quorum(mninfos_valid=others)
        for mn in cloners:
            mn.get_node(self).quorum('dkgsimerror', 'contribution-clone', 0)

        valid_members = {m['proTxHash'] for m in self.nodes[0].quorum('info', 100, qh)['members'] if m['valid']}
        assert_equal(valid_members, {mn.proTxHash for mn in others})
        for mn in cloners:
            assert check_punished(self.nodes[0], mn)
            assert not check_banned(self.nodes[0], mn)

        self.log.info("Let the PoSe penalty of the cloners decay without running any DKG")
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 4070908800)
        self.wait_for_sporks_same()
        self.generate(self.nodes[0], 66)
        for mn in self.mninfo:
            assert not check_punished(self.nodes[0], mn)
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

    def isolate_mn(self, mn: MasternodeInfo):
        mn.get_node(self).setnetworkactive(False)
        self.wait_until(lambda: mn.get_node(self).getconnectioncount() == 0)
        return True, True

    def close_mn_port(self, mn: MasternodeInfo):
        self.deaf_mns.append(mn)
        self.stop_node(mn.nodeIdx)
        self.start_masternode(mn, ["-listen=0", "-nobind"])
        self.connect_nodes(mn.nodeIdx, 0)
        # Make sure the to-be-banned node is still connected well via outbound connections
        for mn2 in self.mninfo: # type: MasternodeInfo
            if self.deaf_mns.count(mn2) == 0:
                self.connect_nodes(mn.nodeIdx, mn2.get_node(self).index)
        self.reset_probe_timeouts()
        return False, False


if __name__ == '__main__':
    LLMQSimplePoSeTest().main()
