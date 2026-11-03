#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test rotating LLMQ quarters that can't be filled.

Three masternodes can't fill the quarters of an llmq_test_dip0024 of size 8 (quarter size 2), so
cycles keep building short and empty quarters. Every rebuilt quarter has to be the one built, and
a quorum keeps the members it actually got: it forms if they meet minSize (3), else it doesn't.
The result has to survive a reorg and a restart.
"""

import re

from test_framework.authproxy import JSONRPCException
from test_framework.masternodes import check_banned
from test_framework.test_framework import DashTestFramework, throttled
from test_framework.util import assert_equal, assert_greater_than, force_finish_mnsync

LLMQ_TYPE = 103
LLMQ_NAME = "llmq_test_dip0024"
LLMQ_SIZE = 8
LLMQ_MIN_SIZE = 3  # -llmqtestdip0024params sets minSize = threshold
QUARTER_SIZE = LLMQ_SIZE // 4
QUORUM_COUNT = 2
CYCLE = 24
WORK_DIFF_DEPTH = 8
MINING_WINDOW_END = 20
MODE_SHORT_QUARTERS = 4
SPORK_OFF = 4070908800
# proTxHash characters in the composition logs
LOG_PREFIX_LEN = 16

LOG_ENTRY_START = re.compile(r"^\d{4}-\d{2}-\d{2}T")
QUARTER_COMPOSITION = re.compile(
    r"QuarterComposition h\[(\d+)\] i\[(\d+)\]: 2Cmns\[([^\]]*)\] 1Cmns\[([^\]]*)\] 0Cmns\[([^\]]*)\] new\[([^\]]*)\]")
CANDIDATES = re.compile(r"BuildNewQuorumQuarterMembers h\[(\d+)\] sortedCombinedMns\[([^\]]*)\]")
DKG_COMPOSITION = re.compile(r"DKGComposition h\[(\d+)\] i\[(\d+)\] DKG:\[([^\]]*)\]")
RECEIVED = {2: "receivedContributions", 3: "receivedComplaints", 4: "receivedJustifications", 5: "receivedPrematureCommitments"}
SENT = {"receivedContributions": "sentContributions", "receivedComplaints": "sentComplaint",
        "receivedJustifications": "sentJustification", "receivedPrematureCommitments": "sentPrematureCommitment"}


def split_prefixes(text, separator):
    return [p.strip() for p in text.split(separator) if p.strip()]


class LogScanner:
    """Collects the debug.log entries of a node that contain marker. A batched log call (CBatchedLogger)
    prefixes only its first line, so an entry is a prefixed line plus the unprefixed lines after it.
    An entry is complete once the next one has started."""

    def __init__(self, path, marker):
        self.path = path
        self.marker = marker
        self.offset = 0
        self.pending = None
        self.entries = []

    def scan(self):
        with open(self.path, "rb") as f:
            f.seek(self.offset)
            data = f.read()
        end = data.rfind(b"\n") + 1
        self.offset += end
        for line in data[:end].decode("utf-8", errors="replace").splitlines():
            if LOG_ENTRY_START.match(line):
                if self.pending is not None and self.marker in self.pending[0]:
                    self.entries.append("\n".join(self.pending))
                self.pending = [line]
            elif self.pending is not None:
                self.pending.append(line)
        return self.entries


class LLMQRotationShortQuartersTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        extra_args = [[f"-llmqtestdip0024params={LLMQ_SIZE}:{LLMQ_MIN_SIZE}"] for _ in range(4)]
        self.set_dash_test_params(4, 3, extra_args=extra_args)
        self.llmq_size_dip0024 = LLMQ_SIZE
        self.llmq_min_size_dip0024 = LLMQ_MIN_SIZE

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # ---- log based views of the member selection ----

    def prefix_map(self):
        """The 16-character proTxHash prefixes used in logs, mapped to the registered proTxHashes"""
        prefixes = {mn.proTxHash[:LOG_PREFIX_LEN]: mn.proTxHash for mn in self.mninfo}
        assert_equal(len(prefixes), len(self.mninfo))
        return prefixes

    def quarter_logs(self):
        """(cycle, index) -> every distinct (2C, 1C, 0C, new) composition node0 logged for it, in proTxHashes"""
        result = {}
        for entry in self.quarter_scanner.scan():
            m = QUARTER_COMPOSITION.search(entry)
            assert m is not None, entry
            quarters = tuple(tuple(self.prefixes[p] for p in split_prefixes(m.group(g), "|")) for g in range(3, 7))
            result.setdefault((int(m.group(1)), int(m.group(2))), [])
            if quarters not in result[(int(m.group(1)), int(m.group(2)))]:
                result[(int(m.group(1)), int(m.group(2)))].append(quarters)
        return result

    def cycle_quarters(self, cycle):
        """Per index, the quarters of the cycle's quorum: built 3, 2 and 1 cycles before and the new one"""
        logs = self.quarter_logs()
        result = []
        for i in range(QUORUM_COUNT):
            assert (cycle, i) in logs, f"no QuarterComposition h[{cycle}] i[{i}]"
            # every computation of a cycle (block processing, cold caches, reorgs) must agree
            assert_equal(len(logs[(cycle, i)]), 1)
            result.append(logs[(cycle, i)][0])
        return result

    def built_quarters(self, cycle):
        """Per index, the quarter the builder selected for the cycle"""
        return [q[3] for q in self.cycle_quarters(cycle)]

    def dkg_logs(self):
        """(cycle, index) -> (members, aborted) of the DKG the logging masternode initialized"""
        result = {}
        for entry in self.dkg_scanner.scan():
            m = DKG_COMPOSITION.search(entry)
            assert m is not None, entry
            members = [self.prefixes[p] for p in split_prefixes(m.group(3), "|")]
            result[(int(m.group(1)), int(m.group(2)))] = (members, "not enough members" in entry)
        return result

    def dkg_composition(self, cycle, index):
        """The members the DKG of the cycle's quorum index ran with, in member order, and whether its init
        was aborted for having fewer than minSize members"""
        self.wait_until(lambda: (cycle, index) in self.dkg_logs())
        return self.dkg_logs()[(cycle, index)]

    # ---- chain helpers ----

    def live_masternodes(self):
        return [mn for mn in self.online if not check_banned(self.nodes[0], mn)]

    def mined_quorum(self, quorum_hash):
        try:
            return self.nodes[0].quorum("info", LLMQ_TYPE, quorum_hash)
        except JSONRPCException as e:
            assert_equal(e.error["message"], "quorum not found")
            return None

    def all_online_nodes(self):
        return [self.nodes[0]] + [mn.get_node(self) for mn in self.online]

    def mine_blocks(self, count):
        self.move_blocks(self.all_online_nodes(), count)

    def wait_for_masternode_connections(self):
        bump = throttled(lambda: self.bump_mocktime(1))

        def connected():
            bump()
            for mn in self.online:
                peers = {p.get("verified_proregtx_hash") for p in mn.get_node(self).getpeerinfo()}
                if any(other.proTxHash not in peers for other in self.online if other is not mn):
                    return False
            return True
        self.wait_until(connected)

    def predictions(self, cycle):
        """'quorum dkginfo' entries of both indexes of the cycle for every registered masternode. Asked on a
        masternode, so that node0 only logs the member selection of block processing."""
        node = self.mninfo[0].get_node(self)
        result = {}
        for mn in self.mninfo:
            entries = [d for d in node.quorum("dkginfo", mn.proTxHash)["upcoming_dkgs"] if d["llmqType"] == LLMQ_TYPE]
            for i in range(QUORUM_COUNT):
                [entry] = [d for d in entries if d["quorumHeight"] == cycle + i]
                result[(mn.proTxHash, i)] = entry
        return result

    # ---- DKG driving ----

    def wait_for_dkg_connections(self, quorum_hash, rotating_members, cycle_base):
        """DKG messages are relayed only once and only to the members connected at that moment, so every live
        member has to be connected to its live outbound relay members before it sends anything"""
        live = {mn.proTxHash for mn in self.live_masternodes()}
        bump = throttled(lambda: self.bump_mocktime(1))

        def check():
            for mn in self.live_masternodes():
                required = {"llmq_test"} if cycle_base else set()
                if mn.proTxHash in rotating_members:
                    required.add(LLMQ_NAME)
                for qc in mn.get_node(self).quorum("dkgstatus")["quorumConnections"]:
                    if qc.get("quorumHash") != quorum_hash or not qc.get("quorumConnections"):
                        continue
                    if qc["llmqType"] == LLMQ_NAME and mn.proTxHash not in rotating_members:
                        continue
                    if any(c["outbound"] and not c["connected"] and c["proTxHash"] in live for c in qc["quorumConnections"]):
                        bump()
                        return False
                    required.discard(qc["llmqType"])
                if required:
                    bump()
                    return False
            return True
        self.wait_until(check)

    def wait_for_dkg_phase(self, quorum_hash, phase, rotating_members, cycle_base):
        """Wait until every live member of every llmq type with a DKG at quorum_hash is in phase and, from phase 2
        on, holds every message of the phase its members report having sent. Of llmq_test_dip0024 only the
        members in rotating_members count. At a cycle base llmq_test runs with every live masternode."""
        received = RECEIVED.get(phase)
        live = {mn.proTxHash for mn in self.live_masternodes()}

        def check():
            members = {}
            statuses = {}
            for mn in self.live_masternodes():
                dkgstatus = mn.get_node(self).quorum("dkgstatus")
                for qc in dkgstatus["quorumConnections"]:
                    if qc.get("quorumHash") == quorum_hash and qc["llmqType"] != LLMQ_NAME:
                        members.setdefault(qc["llmqType"], set()).update(c["proTxHash"] for c in qc["quorumConnections"])
                for qs in dkgstatus["session"]:
                    if qs["status"]["quorumHash"] != quorum_hash:
                        continue
                    if qs["llmqType"] == LLMQ_NAME and mn.proTxHash not in rotating_members:
                        continue
                    statuses.setdefault(qs["llmqType"], []).append(qs["status"])
            if rotating_members:
                members[LLMQ_NAME] = set(rotating_members)
            if cycle_base:
                members["llmq_test"] = members.get("llmq_test", set()) | live
            for llmq_type, type_members in members.items():
                type_statuses = statuses.get(llmq_type, [])
                if len(type_statuses) < len(type_members & live):
                    return False
                if any(s["phase"] != phase for s in type_statuses):
                    return False
                if received is not None:
                    expected = sum(s[SENT[received]] for s in type_statuses)
                    if any(s[received] < expected for s in type_statuses):
                        return False
            return True
        self.wait_until(check)

    def set_dkg_enabled(self, enabled):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0 if enabled else SPORK_OFF)
        self.wait_for_sporks_same()

    def mine_rotation_cycle(self, on_composition=None):
        """Drive the next DKG cycle and return its llmq_test_dip0024 quorums by index, None for an index that
        mined none. on_composition(cycle, index, members) runs as soon as an index's DKG has initialized,
        before anything waits on it. llmq_test and the other non-rotating types run with the live masternodes.
        Every rotating index whose composition meets minSize gets its connections, each phase and its messages
        before the chain moves on, and has to mine a commitment if its live members reach the threshold.
        'quorum dkginfo' must have predicted both indexes' members."""
        node = self.nodes[0]
        height = node.getblockcount()
        cycle = height + CYCLE - height % CYCLE
        if cycle - 1 > height:
            self.mine_blocks(cycle - 1 - height)
        if node.spork("show")["SPORK_17_QUORUM_DKG_ENABLED"] != 0:
            self.set_dkg_enabled(True)
        predictions = self.predictions(cycle)
        self.mine_blocks(1)
        assert_equal(node.getblockcount(), cycle)

        live = {mn.proTxHash for mn in self.live_masternodes()}
        compositions = {}
        rotating = {}
        forms = {}
        for step in range(12):
            index = step % 2
            quorum_hash = node.getblockhash(cycle + index)
            phase = step // 2 + 1
            if step < QUORUM_COUNT:
                members, aborted = self.dkg_composition(cycle, index)
                compositions[index] = members
                # a DKG with fewer members than minSize never starts
                assert_equal(aborted, len(members) < LLMQ_MIN_SIZE)
                if on_composition is not None:
                    on_composition(cycle, index, members)
                rotating[index] = set() if aborted else set(members)
                forms[index] = not aborted and len(set(members) & live) >= LLMQ_MIN_SIZE
                self.wait_for_dkg_phase(quorum_hash, phase, rotating[index], index == 0)
                self.wait_for_dkg_connections(quorum_hash, rotating[index], index == 0)
            elif index == 0 or rotating[index]:
                self.wait_for_dkg_phase(quorum_hash, phase, rotating[index], index == 0)
            if step == 10:
                self.wait_for_quorum_commitment(quorum_hash, self.live_masternodes(), llmq_type=100)
                self.wait_for_quorum_commitments_on_miner(quorum_hash, self.live_masternodes())
            if step == 11:
                for i in range(QUORUM_COUNT):
                    if forms[i]:
                        self.wait_for_quorum_commitment(node.getblockhash(cycle + i), self.live_masternodes(), llmq_type=LLMQ_TYPE)
                        self.wait_for_quorum_commitments_on_miner(node.getblockhash(cycle + i), self.live_masternodes())
            self.mine_blocks(1)

        # past the mining window every commitment that was going to be mined is
        self.mine_blocks(cycle + MINING_WINDOW_END + 1 - node.getblockcount())
        quorums = {}
        for i in range(QUORUM_COUNT):
            quorums[i] = self.mined_quorum(node.getblockhash(cycle + i))
            assert_equal(quorums[i] is not None, forms[i])
            if quorums[i] is not None:
                assert_equal([m["proTxHash"] for m in quorums[i]["members"]], compositions[i])
                self.recorded[quorums[i]["quorumHash"]] = quorums[i]["members"]
        self.compositions[cycle] = compositions
        self.log.info(f"Cycle {cycle}: " +
                      ", ".join(f"i[{i}] DKG:{[p[:4] for p in m]} quorum:{quorums[i] is not None}" for i, m in compositions.items()))

        for (proTxHash, i), entry in predictions.items():
            assert entry["known"], entry
            assert_equal(entry["workBlockHash"], node.getblockhash(cycle - WORK_DIFF_DEPTH))
            assert_equal(entry["isMember"], proTxHash in compositions[i])
        return quorums

    # ---- checks ----

    def run_test(self):
        node = self.nodes[0]
        self.prefixes = self.prefix_map()
        self.quarter_scanner = LogScanner(node.debug_log_path, "QuarterComposition")
        self.dkg_scanner = LogScanner(self.mninfo[0].get_node(self).debug_log_path, "DKGComposition")
        self.online = self.mninfo.copy()
        self.recorded = {}
        self.compositions = {}

        # no ChainLocks, so the cycles can be reorged, and no InstantSend
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        node.sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", SPORK_OFF)
        node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", SPORK_OFF)
        self.wait_for_sporks_same()

        self.test_short_quorums()
        self.test_below_minimum()
        self.test_reorg()
        self.test_restart()

    def check_quarters(self, cycle, index, members):
        """The oracle: every quarter of a cycle's quorum, as its DKG used it, is the quarter built for it"""
        quarters = self.cycle_quarters(cycle)[index]
        logs = self.quarter_logs()
        for k in (3, 2, 1):
            built_at = cycle - k * CYCLE
            if (built_at, index) in logs:
                built = self.built_quarters(built_at)[index]
                assert_equal(quarters[3 - k], built)
                if len(built) < QUARTER_SIZE:
                    self.short_rebuilds += 1
        assert_equal(len(set(members)), len(members))
        assert_equal(members, [m for quarter in quarters for m in quarter])

    def check_short_quorums(self, quorums):
        for quorum in quorums.values():
            if quorum is None:
                continue
            members = [m["proTxHash"] for m in quorum["members"]]
            assert_equal(len(set(members)), len(members))
            if len(members) < LLMQ_SIZE:
                self.short_quorums += 1

    def snapshot_modes(self):
        """The skip list modes of the snapshots 'quorum rotationinfo' (qrinfo) serves for the tip's cycles"""
        info = self.nodes[0].quorum("rotationinfo", self.nodes[0].getbestblockhash(), True, [])
        snapshots = [info[f"quorumSnapshotAtHMinus{k}C"] for k in ("", "2", "3", "4")]
        return {s["mnSkipListMode"] for s in snapshots}

    def test_short_quorums(self):
        self.log.info("Rebuilt quarters are the ones built, no quorum repeats a member and short quorums form")
        self.mine_until_mns_confirmed_for_next_dkg()
        self.first_cycle = None
        self.short_rebuilds = 0
        self.short_quorums = 0
        modes = set()
        for _ in range(5):
            self.check_short_quorums(self.mine_rotation_cycle(on_composition=self.check_quarters))
            if self.first_cycle is None:
                self.first_cycle = max(self.compositions)
            modes |= self.snapshot_modes()
        # cycles with short quarters are stored in their own mode
        assert MODE_SHORT_QUARTERS in modes, modes
        # a short quarter is where the legacy rebuild would have padded
        assert_greater_than(self.short_rebuilds, 0)
        assert_greater_than(self.short_quorums, 0)

    def test_below_minimum(self):
        self.log.info("A quorum below minSize does not form")
        node = self.nodes[0]
        # as feature_llmq_simplepose.py: isolate a masternode until it is PoSe-banned
        mn = self.mninfo[2]
        mn.get_node(self).setnetworkactive(False)
        self.wait_until(lambda: mn.get_node(self).getconnectioncount() == 0)
        self.online.remove(mn)
        for _ in range(3):
            self.check_short_quorums(self.mine_rotation_cycle(on_composition=self.check_quarters))
            if check_banned(node, mn):
                break
        assert check_banned(node, mn)

        below_minimum = None
        for _ in range(6):
            self.check_short_quorums(self.mine_rotation_cycle(on_composition=self.check_quarters))
            cycle = max(self.compositions)
            for i, members in self.compositions[cycle].items():
                if len(members) < LLMQ_MIN_SIZE:
                    # mine_rotation_cycle checked that its init aborted and no commitment was mined
                    below_minimum = (cycle, i)
            if below_minimum is not None:
                break
        assert below_minimum is not None, "no below-minimum cycle"
        self.log.info(f"Below minimum: {below_minimum}")

    def check_recorded_quorums(self):
        """Every recorded quorum still has its members"""
        for quorum_hash, members in self.recorded.items():
            info = self.nodes[0].quorum("info", LLMQ_TYPE, quorum_hash)
            assert_equal([(m["proTxHash"], m["valid"]) for m in info["members"]],
                         [(m["proTxHash"], m["valid"]) for m in members])

    def check_recomputed_quarters(self, quarter_entries):
        """Every cycle's quarters were computed again since the first quarter_entries log entries, and every
        computation of a cycle agreed with the earlier ones (cycle_quarters checks)"""
        recomputed = {int(QUARTER_COMPOSITION.search(e).group(1)) for e in self.quarter_scanner.scan()[quarter_entries:]}
        assert set(self.compositions) <= recomputed, set(self.compositions) - recomputed
        for cycle in self.compositions:
            self.cycle_quarters(cycle)

    def reorg_cycles(self):
        """Disconnect every block from the first driven cycle's base on and connect them again"""
        nodes = self.all_online_nodes()
        tip = self.nodes[0].getbestblockhash()
        first = self.nodes[0].getblockhash(self.first_cycle)
        for n in nodes:
            n.invalidateblock(first)
        for n in nodes:
            assert_equal(n.getblockcount(), self.first_cycle - 1)
        for n in nodes:
            n.reconsiderblock(first)
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in nodes))

    def test_reorg(self):
        self.log.info("A reorg of every driven cycle keeps every member list")
        quarter_entries = len(self.quarter_scanner.scan())
        self.reorg_cycles()
        self.check_recorded_quorums()
        self.check_recomputed_quarters(quarter_entries)

    def test_restart(self):
        self.log.info("A restart (cold caches) keeps every member list")
        quarter_entries = len(self.quarter_scanner.scan())
        self.restart_node(0, extra_args=self.extra_args[0])
        force_finish_mnsync(self.nodes[0])
        self.restart_masternodes(mns=self.online)
        # 'quorum info' rebuilds the recorded quorums from the stored snapshots
        self.check_recorded_quorums()
        # quorums that never formed are only rebuilt when their cycle is disconnected and connected again
        self.reorg_cycles()
        self.check_recorded_quorums()
        self.check_recomputed_quarters(quarter_entries)


if __name__ == '__main__':
    LLMQRotationShortQuartersTest().main()
