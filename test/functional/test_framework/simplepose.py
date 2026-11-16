#!/usr/bin/env python3
# Copyright (c) 2015-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Shared setup and PoSe banning helpers for the feature_llmq_simplepose*.py tests."""

from test_framework.masternodes import check_banned, check_punished
from test_framework.test_framework import MasternodeInfo
from test_framework.util import assert_equal, force_finish_mnsync


class SimplePoSeMixin:
    """Mixed into the DashTestFramework subclasses of the feature_llmq_simplepose*.py tests"""
    def set_simplepose_test_params(self):
        # rotating quorums add instability for this functional tests
        self.extra_args = [[ '-testactivationheight=dip0024@9999' ]] * 6
        self.set_dash_test_params(6, 5)
        self.set_dash_llmq_test_params(5, 3)
        self.delay_v20_and_mn_rr(height=9999)
        self.deaf_mns = []

    def add_options(self, parser):
        self.add_wallet_options(parser)
        parser.add_argument("--disable-spork23", dest="disable_spork23", default=False, action="store_true",
                            help="Test with spork23 disabled")

    def set_pose_sporks(self):
        if self.options.disable_spork23:
            self.nodes[0].sporkupdate("SPORK_23_QUORUM_POSE", 4070908800)
        else:
            self.nodes[0].sporkupdate("SPORK_23_QUORUM_POSE", 0)

        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

    def test_no_banning(self, invalidate_proc, expected_connections=None):
        [_, instant_ban] = invalidate_proc(self.mninfo[0])
        iters = 2 if instant_ban else 6
        for i in range(iters):
            self.log.info(f"Testing no PoSe banning in normal conditions {i + 1}/{iters}")
            self.mine_quorum(expected_connections=expected_connections)

    def mine_quorum_less_checks(self, expected_members, mninfos_online):
        # Unlike in mine_quorum we don't know how many complaints and justifications to expect:
        # the deaf masternode is a regular DKG participant until enough probes to it have failed,
        # so each phase waits for exactly the messages the members report having sent instead.
        self.log.info("Mining a quorum with less checks")
        nodes = [self.nodes[0]] + [mn.get_node(self) for mn in mninfos_online]

        # move forward to next DKG
        skip_count = 24 - (self.nodes[0].getblockcount() % 24)
        if skip_count != 0:
            self.bump_mocktime(skip_count, nodes=nodes)
            self.generate(self.nodes[0], skip_count, sync_fun=lambda: self.sync_blocks(nodes))

        q = self.nodes[0].getbestblockhash()
        self.log.info("Expected quorum_hash: "+str(q))
        self.log.info("Waiting for phase 1 (init)")
        self.wait_for_quorum_phase(q, 1, expected_members, None, 0, mninfos_online, all_types=True)
        self.wait_for_quorum_connections(q, expected_members - 1, mninfos_online, wait_proc=lambda: self.bump_mocktime(1), all_types=True)
        self.log.info("Waiting for this round's probes of the deaf masternodes to be attempted")
        self.wait_for_deaf_masternode_probes(mninfos_online)
        self.move_blocks(nodes, 2)

        self.log.info("Waiting for phase 2 (contribute)")
        self.wait_for_quorum_phase(q, 2, expected_members, "receivedContributions", expected_members, mninfos_online, all_types=True)
        self.wait_for_offset_quorum_phase(q, 2, mninfos_online)
        self.move_blocks(nodes, 2)

        self.log.info("Waiting for phase 3 (complain)")
        self.wait_for_quorum_phase(q, 3, expected_members, "receivedComplaints", None, mninfos_online, all_types=True)
        self.wait_for_offset_quorum_phase(q, 3, mninfos_online)
        self.move_blocks(nodes, 2)

        self.log.info("Waiting for phase 4 (justify)")
        self.wait_for_quorum_phase(q, 4, expected_members, "receivedJustifications", None, mninfos_online, all_types=True)
        self.wait_for_offset_quorum_phase(q, 4, mninfos_online)
        self.move_blocks(nodes, 2)

        self.log.info("Waiting for phase 5 (commit)")
        self.wait_for_quorum_phase(q, 5, expected_members, "receivedPrematureCommitments", expected_members, mninfos_online, all_types=True)
        self.wait_for_offset_quorum_phase(q, 5, mninfos_online)
        self.move_blocks(nodes, 2)

        self.log.info("Waiting for phase 6 (mining)")
        self.wait_for_quorum_phase(q, 6, expected_members, None, 0, mninfos_online, all_types=True)
        self.wait_for_offset_quorum_phase(q, 6, mninfos_online)

        self.log.info("Waiting final commitment")
        self.wait_for_quorum_commitment(q, mninfos_online)

        self.log.info("Waiting final commitments on mining node")
        self.wait_for_quorum_commitments_on_miner(q, mninfos_online)

        self.log.info("Mining final commitment")
        self.bump_mocktime(1, nodes=nodes)
        self.nodes[0].getblocktemplate() # this calls CreateNewBlock
        self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks(nodes))

        self.log.info("Waiting for quorum to appear in the list")
        self.wait_for_quorum_list(q, nodes)

        new_quorum = self.nodes[0].quorum("list", 1)["llmq_test"][0]
        assert_equal(q, new_quorum)
        quorum_info = self.nodes[0].quorum("info", 100, new_quorum)

        self.wait_for_offset_quorum_commitments(q, mninfos_online)

        # Mine 8 (SIGN_HEIGHT_OFFSET) more blocks to make sure that the new quorum gets eligible for signing sessions
        self.bump_mocktime(8)
        self.generate(self.nodes[0], 8, sync_fun=lambda: self.sync_blocks(nodes))
        self.log.info("New quorum: height=%d, quorumHash=%s, quorumIndex=%d, minedBlock=%s" % (quorum_info["height"], new_quorum, quorum_info["quorumIndex"], quorum_info["minedBlock"]))

        return new_quorum

    def wait_for_deaf_masternode_probes(self, mninfos_online):
        # A member marks a deaf masternode bad only once enough probes of it have failed, and a
        # probe attempt is counted when the connection is opened, not when the DKG round starts.
        # reset_probe_timeouts() moved mocktime 10 minutes ahead of any previous attempt, so a
        # younger attempt is this round's one. Already banned deaf masternodes are no members
        # and are not probed anymore.
        probed_mns = [mn for mn in self.deaf_mns if not check_banned(self.nodes[0], mn)]

        def probes_attempted():
            for mn in mninfos_online:
                if mn in self.deaf_mns:
                    continue
                for probed_mn in probed_mns:
                    if mn.get_node(self).protx('info', probed_mn.proTxHash)['metaInfo']['lastOutboundAttemptElapsed'] > 10 * 60:
                        return False
            return True

        self.wait_until(probes_attempted)

    def test_banning(self, invalidate_proc, expected_connections=None):
        mninfos_online = self.mninfo.copy()
        mninfos_valid = self.mninfo.copy()

        for mn in mninfos_valid:
            assert not check_punished(self.nodes[0], mn)
            assert not check_banned(self.nodes[0], mn)

        expected_contributors = len(mninfos_online)
        for i in range(2):
            self.log.info(f"Testing PoSe banning due to {invalidate_proc.__name__} {i + 1}/2")
            mn: MasternodeInfo = mninfos_valid.pop()
            went_offline, instant_ban = invalidate_proc(mn)
            expected_complaints = expected_contributors - 1
            if went_offline:
                mninfos_online.remove(mn)
                expected_contributors -= 1

            # NOTE: Min PoSe penalty is 100 (see CDeterministicMNList::CalcMaxPoSePenalty()),
            # so nodes are PoSe-banned in the same DKG they misbehave without being PoSe-punished first.
            if instant_ban:
                assert expected_connections is not None
                self.log.info("Expecting instant PoSe banning")
                self.reset_probe_timeouts()
                self.mine_quorum(expected_connections=expected_connections, expected_members=expected_contributors, expected_contributions=expected_contributors, expected_complaints=expected_complaints, expected_commitments=expected_contributors, mninfos_online=mninfos_online, mninfos_valid=mninfos_valid)

                if not check_banned(self.nodes[0], mn):
                    self.log.info("Instant ban still requires 2 missing DKG round. If it is not banned yet, mine 2nd one")
                    self.reset_probe_timeouts()
                    self.mine_quorum(expected_connections=expected_connections, expected_members=expected_contributors, expected_contributions=expected_contributors, expected_complaints=expected_complaints, expected_commitments=expected_contributors, mninfos_online=mninfos_online, mninfos_valid=mninfos_valid)
            else:
                # A member is marked bad once more than MASTERNODE_MAX_FAILED_OUTBOUND_ATTEMPTS (5)
                # probes of it failed, i.e. from the 6th DKG round on with one probe per round, and
                # each exclusion costs 66% of the ban threshold, so the 7th round bans it for sure.
                assert expected_connections is None
                for j in range(7):
                    self.log.info(f"Accumulating PoSe penalty {j + 1}/7")
                    self.reset_probe_timeouts()
                    self.mine_quorum_less_checks(expected_contributors, mninfos_online)
                    if check_banned(self.nodes[0], mn):
                        break

            assert check_banned(self.nodes[0], mn)

            if not went_offline:
                # we do not include PoSe banned mns in quorums, so the next one should have 1 contributor less
                expected_contributors -= 1

    def repair_masternodes(self, restart):
        self.log.info("Repairing all banned and punished masternodes")
        repaired = [mn for mn in self.mninfo if check_banned(self.nodes[0], mn) or check_punished(self.nodes[0], mn)]
        for mn in repaired:
            addr = self.nodes[0].getnewaddress()
            self.nodes[0].sendtoaddress(addr, 0.1)
            mn.update_service(self.nodes[0], submit=True, fundsAddr=addr)
        if restart:
            self.restart_masternodes(mns=repaired)
        else:
            for mn in repaired:
                mn.get_node(self).setnetworkactive(True)
                self.connect_nodes(mn.nodeIdx, 0)

        # syncing blocks only since node 0 has txes waiting to be mined
        self.sync_blocks()

        # Make sure protxes are "safe" to mine even when InstantSend and ChainLocks are no longer functional
        self.bump_mocktime(60 * 10 + 1)
        self.generate(self.nodes[0], 1)

        # Isolate and re-connect all MNs (otherwise there might be open connections with no MNAUTH for MNs which were banned before)
        for mn in self.mninfo: # type: MasternodeInfo
            assert not check_banned(self.nodes[0], mn)
            mn.get_node(self).setnetworkactive(False)
            self.wait_until(lambda: mn.get_node(self).getconnectioncount() == 0)
            # node0 has to notice the drop as well, or it rejects the fresh MNAUTH as a duplicate of the stale peer
            self.wait_until(lambda: all(p.get("verified_proregtx_hash") != mn.proTxHash for p in self.nodes[0].getpeerinfo()))
            mn.get_node(self).setnetworkactive(True)
            force_finish_mnsync(mn.get_node(self))
            self.connect_nodes(mn.nodeIdx, 0)

    def reset_probe_timeouts(self):
        # Make sure all masternodes will reconnect/re-probe
        self.bump_mocktime(10 * 60 + 1)
