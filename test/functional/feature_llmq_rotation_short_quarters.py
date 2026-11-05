#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test rotating LLMQ quarters across the activation of quarter_steps.

Three masternodes can't fill the quarters of an llmq_test_dip0024 of size 8 (quarter size 2), so
cycles keep building short and empty quarters. Before quarter_steps their snapshots can't say so
and rebuilding pads them, repeating members. After it, every rebuilt quarter is the one built, and
a quorum keeps the members it actually got: it forms if they meet minSize (3), else it doesn't.
The deployment is activated through EHF, the cycle base block decides which rule a cycle uses,
and the result has to survive a reorg across the activation and a restart.
"""

from functools import partial
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
QUARTER_STEPS_BIT = 15
EHF_WINDOW = 5
FAR_FUTURE = 999999999999
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
        extra_args = [[f"-vbparams=quarter_steps:{FAR_FUTURE - 1}:{FAR_FUTURE}",
                       f"-llmqtestdip0024params={LLMQ_SIZE}:{LLMQ_MIN_SIZE}",
                       "-persistmempool=0"] for _ in range(4)]
        self.set_dash_test_params(4, 3, extra_args=extra_args)
        self.quarter_steps_always_active = False
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

    def candidates(self, cycle):
        """The candidate list the builder of the cycle walked, in proTxHashes"""
        lists = {tuple(self.prefixes[p] for p in split_prefixes(m.group(2), "|"))
                 for m in map(CANDIDATES.search, self.candidate_scanner.scan()) if m and int(m.group(1)) == cycle}
        assert_equal(len(lists), 1)
        return list(lists.pop())

    def legacy_quarters(self, cycle):
        """Per index, the new quarter of a legacy cycle as the legacy rebuild returns it: the legacy builder walk
        (WalkLegacyQuarters) over the cycle's candidates, then the legacy replay (ReplayLegacyQuarters) of the
        skipped positions it stores. Before any PoSe ban, the members of an index's previous quarters are the
        ones it can't use."""
        candidates = self.candidates(cycle)
        n = len(candidates)
        built = [[] for _ in range(QUORUM_COUNT)]
        skips = []
        idx = 0
        for i, quarters in enumerate(self.cycle_quarters(cycle)):
            used = {m for quarter in quarters[:3] for m in quarter}
            used_count = len(used)
            updated = False
            start = idx
            while len(built[i]) < QUARTER_SIZE and used_count + len(built[i]) < n:
                if candidates[idx] in used:
                    skips.append(idx)
                else:
                    used.add(candidates[idx])
                    built[i].append(candidates[idx])
                    updated = True
                idx = (idx + 1) % n
                if idx == start:
                    if not updated:
                        # no snapshot is stored, so later cycles rebuild nothing
                        assert_equal(self.built_quarters(cycle), [()] * QUORUM_COUNT)
                        return [()] * QUORUM_COUNT
                    updated = False
        assert_equal([tuple(b) for b in built], self.built_quarters(cycle))
        rebuilt = [[] for _ in range(QUORUM_COUNT)]
        idx = 0
        for i in range(QUORUM_COUNT):
            while len(rebuilt[i]) < QUARTER_SIZE:
                if skips and idx == skips[0]:
                    skips.pop(0)
                else:
                    rebuilt[i].append(candidates[idx])
                idx = (idx + 1) % n
        return [tuple(r) for r in rebuilt]

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

    def quarter_steps_bip9(self, height):
        return self.nodes[0].getdeploymentinfo(self.nodes[0].getblockhash(height))["deployments"]["quarter_steps"]["bip9"]

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

    def mine_rotation_cycle(self, mode, on_composition=None):
        """Drive the next DKG cycle and return its llmq_test_dip0024 quorums by index, None for an index that
        mined none. on_composition(cycle, index, members) runs as soon as an index's DKG has initialized,
        before anything waits on it. llmq_test and the other non-rotating types run with the live masternodes.

        mode "legacy": the rotating sessions are left alone (they may abort or fail). A rotating DKG with a
        repeated member can still commit, and each repeat is then PoSe-punished like a member that did not
        take part, which can ban one of the three masternodes. So once a composition repeats a member, every
        DKG of the cycle is stopped (SPORK_17 off) before any contribution is sent, and none commits.

        mode "faithful": every rotating index whose composition meets minSize gets its connections, each phase
        and its messages before the chain moves on, and has to mine a commitment if its live members reach
        the threshold. 'quorum dkginfo' must have predicted both indexes' members."""
        assert mode in ("legacy", "faithful")
        node = self.nodes[0]
        height = node.getblockcount()
        cycle = height + CYCLE - height % CYCLE
        if cycle - 1 > height:
            self.mine_blocks(cycle - 1 - height)
        if node.spork("show")["SPORK_17_QUORUM_DKG_ENABLED"] != 0:
            self.set_dkg_enabled(True)
        predictions = self.predictions(cycle) if mode == "faithful" else None
        self.mine_blocks(1)
        assert_equal(node.getblockcount(), cycle)

        live = {mn.proTxHash for mn in self.live_masternodes()}
        compositions = {}
        rotating = {}
        forms = {}
        stopped = False
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
                runs = mode == "faithful" and not aborted
                rotating[index] = set(members) if runs else set()
                forms[index] = runs and len(set(members) & live) >= LLMQ_MIN_SIZE
                self.wait_for_dkg_phase(quorum_hash, phase, rotating[index], index == 0)
                self.wait_for_dkg_connections(quorum_hash, rotating[index], index == 0)
                # contributions are sent from the next block on
                if step == QUORUM_COUNT - 1 and mode == "legacy" and \
                        any(len(set(m)) < len(m) for m in compositions.values() if len(m) >= LLMQ_MIN_SIZE):
                    self.set_dkg_enabled(False)
                    stopped = True
                    break
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
            if stopped or len(compositions[i]) < LLMQ_MIN_SIZE or mode == "faithful":
                assert_equal(quorums[i] is not None, forms[i] and not stopped)
            if quorums[i] is not None:
                assert_equal([m["proTxHash"] for m in quorums[i]["members"]], compositions[i])
                self.recorded[quorums[i]["quorumHash"]] = quorums[i]["members"]
        self.compositions[cycle] = compositions
        self.log.info(f"Cycle {cycle} ({mode}{', DKGs stopped' if stopped else ''}): " +
                      ", ".join(f"i[{i}] DKG:{[p[:4] for p in m]} quorum:{quorums[i] is not None}" for i, m in compositions.items()))

        if predictions is not None:
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
        self.candidate_scanner = LogScanner(node.debug_log_path, "sortedCombinedMns[")
        self.dkg_scanner = LogScanner(self.mninfo[0].get_node(self).debug_log_path, "DKGComposition")
        self.online = self.mninfo.copy()
        self.recorded = {}
        self.compositions = {}

        # as in feature_mnehf.py: no ChainLocks, so the activation can be reorged, and no InstantSend
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        node.sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", SPORK_OFF)
        node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", SPORK_OFF)
        self.wait_for_sporks_same()

        self.test_legacy()
        activation = self.test_activation()
        self.test_transition(activation)
        self.test_short_quorums(activation)
        self.test_below_minimum(activation)
        self.test_reorg(activation)
        self.test_restart(activation)

    def test_legacy(self):
        self.log.info("Before quarter_steps rebuilt quarters are padded, repeating members")
        self.mine_until_mns_confirmed_for_next_dkg()
        below_minimum = None
        repeat = None
        for _ in range(4):
            self.mine_rotation_cycle("legacy")
            cycle = max(self.compositions)
            for i, members in self.compositions[cycle].items():
                if len(members) < LLMQ_MIN_SIZE and below_minimum is None:
                    # mine_rotation_cycle checked that its init aborted and no commitment was mined
                    below_minimum = (cycle, i)
                if len(set(members)) < len(members) and repeat is None:
                    repeat = (cycle, i)
            if below_minimum is not None and repeat is not None:
                break
        assert below_minimum is not None, "no below-minimum cycle"
        assert repeat is not None, "no repeated member"
        self.log.info(f"Below minimum: {below_minimum}, repeated member: {repeat}")

        # a fresh build never selects a member of the index's previous quarters, so the repeat comes from a
        # previous quarter rebuilt with more members than its cycle selected
        cycle, i = repeat
        quarters = self.cycle_quarters(cycle)[i]
        padded = [k for k in (1, 2, 3) if len(quarters[3 - k]) > len(self.built_quarters(cycle - k * CYCLE)[i])]
        assert padded, quarters

    def ehf_signals_in_mempool(self, node):
        return [txid for txid in node.getrawmempool()
                if node.getrawtransaction(txid, True).get("mnhfTx", {}).get("signal", {}).get("versionBit") == QUARTER_STEPS_BIT]

    def check_unknown_predictions(self, seen):
        """While the deployment state at a cycle's work block is STARTED or LOCKED_IN, the rule the cycle will use
        is not settled yet, so its members can't be predicted"""
        node = self.mninfo[0].get_node(self)
        for mn in self.mninfo:
            for entry in node.quorum("dkginfo", mn.proTxHash)["upcoming_dkgs"]:
                if entry["llmqType"] != LLMQ_TYPE:
                    continue
                work_height = entry["quorumHeight"] - entry["quorumIndex"] - WORK_DIFF_DEPTH
                state = self.quarter_steps_bip9(work_height)["status_next"]
                # the 5-block window lets STARTED reach ACTIVE within WORK_DIFF_DEPTH blocks
                if state in ("started", "locked_in"):
                    assert not entry["known"], entry
                    assert_equal(entry["reason"], "quarter_steps deployment state at the cycle base is not known yet")
                    seen.add(state)

    def test_activation(self):
        node = self.nodes[0]
        # The first active block A+1 has to start an EHF window (5 blocks) and A has to be a cycle base, i.e.
        # A % 120 == 24. A is far enough ahead to restart the nodes, signal and lock in before it.
        tip = node.getblockcount()
        activation = tip + 48
        activation += (24 - activation % 120) % 120
        assert_equal(activation % CYCLE, 0)
        assert_equal((activation + 1) % EHF_WINDOW, 0)
        self.log.info(f"Activate quarter_steps through EHF at height {activation + 1}")

        # Pre-activation DKGs between check 1 and A+24 are not looked at; with DKGs off the chain can be
        # moved block by block to land the signal and to check every block's predictions
        self.set_dkg_enabled(False)
        self.mine_blocks(activation - 44 - tip)

        # as feature_evo_shares_activation.py: restart every node with the deployment's new -vbparams
        start = self.mocktime + 1
        for args in self.extra_args:
            [pos] = [k for k, arg in enumerate(args) if arg.startswith("-vbparams=quarter_steps:")]
            args[pos] = f"-vbparams=quarter_steps:{start}:{FAR_FUTURE}:{activation + 1}:{EHF_WINDOW}:5:5:5:1"
        self.restart_node(0, extra_args=self.extra_args[0])
        force_finish_mnsync(node)
        self.restart_masternodes()
        self.wait_for_sporks_same()
        assert_equal(node.spork("show")["SPORK_17_QUORUM_DKG_ENABLED"], SPORK_OFF)
        assert_equal(get_bip9_status(node), "defined")
        # the members of the signing quorum exchange their shares over their quorum connections
        self.wait_for_masternode_connections()

        # Masternodes sign the deployment's bit once its signal window opens: an llmq_test quorum recovers a
        # signature and its mnhf tx goes to the mempool
        self.bump_mocktime(1)
        while node.getblockheader(node.getbestblockhash())["mediantime"] < start:
            self.mine_blocks(1)
        bump = throttled(lambda: self.bump_mocktime(1))
        self.wait_until(lambda: bump() or len(self.ehf_signals_in_mempool(node)) == 1)
        [signal_txid] = self.ehf_signals_in_mempool(node)
        signal_height = node.getblockcount() + 1
        # mined at A-39 .. A-35, the deployment is STARTED for the window A-34 .. A-30, which holds the block after
        # the work block of cycle A-24, and LOCKED_IN from A-29, which holds the one of cycle A
        assert activation - 39 <= signal_height <= activation - 35, signal_height
        self.mine_blocks(1)
        assert signal_txid in node.getblock(node.getbestblockhash())["tx"]
        signal = node.getrawtransaction(signal_txid, True)["mnhfTx"]["signal"]
        # signed by a non-rotating quorum
        node.quorum("info", 100, signal["quorumHash"])

        states = {}
        seen = set()
        while True:
            height = node.getblockcount()
            states[height] = self.quarter_steps_bip9(height)["status"]
            self.check_unknown_predictions(seen)
            if height == activation + 1:
                break
            self.mine_blocks(1)
        self.log.info(f"States: {states}")
        assert_equal(states[signal_height], "defined")
        assert "started" in states.values()
        assert_equal(self.quarter_steps_bip9(activation)["status"], "locked_in")
        assert_equal(self.quarter_steps_bip9(activation)["status_next"], "active")
        assert_equal(self.quarter_steps_bip9(activation + 1)["status"], "active")
        assert_equal(seen, {"started", "locked_in"})
        return activation

    def legacy_rebuild(self, built_at, activation, index):
        """How the quarter built at built_at < A appeared in the pre-activation cycles that reused it"""
        appearances = [self.cycle_quarters(cycle)[index][3 - (cycle - built_at) // CYCLE]
                       for cycle in range(built_at + CYCLE, min(built_at + 3 * CYCLE, activation) + 1, CYCLE)]
        assert appearances
        assert all(a == appearances[0] for a in appearances), appearances
        return appearances[0]

    def check_quarters(self, activation, cycle, index, members):
        """The oracle: the quarters of a post-activation cycle, as its DKG used them"""
        assert cycle >= activation + CYCLE
        quarters = self.cycle_quarters(cycle)[index]
        for k in (3, 2, 1):
            quarter = quarters[3 - k]
            built_at = cycle - k * CYCLE
            if built_at >= activation + CYCLE:
                built = self.built_quarters(built_at)[index]
                assert_equal(quarter, built)
                if len(built) < QUARTER_SIZE:
                    self.short_rebuilds += 1
            elif built_at < activation:
                assert_equal(quarter, self.legacy_rebuild(built_at, activation, index))
            else:
                # built by the last legacy cycle A and only ever rebuilt after activation, still with the legacy
                # rule
                assert_equal(quarter, self.legacy_quarters(built_at)[index])
                assert_equal(quarter, self.quarter_a.setdefault(index, quarter))
        assert_equal(members, [m for quarter in quarters for m in quarter])

    def test_transition(self, activation):
        self.log.info("Quarters built after activation are rebuilt as built, older ones as before")
        self.short_rebuilds = 0
        self.quarter_a = {}
        oracle = partial(self.check_quarters, activation)
        # These quorums still hold quarters built before activation, which may repeat members, so their
        # rotating DKGs are not waited on
        for cycle in range(activation + CYCLE, activation + 4 * CYCLE, CYCLE):
            self.mine_rotation_cycle("legacy", on_composition=oracle)
            assert_equal(max(self.compositions), cycle)
        self.mine_rotation_cycle("faithful", on_composition=oracle)
        assert_equal(max(self.compositions), activation + 4 * CYCLE)
        assert_equal(set(self.quarter_a), set(range(QUORUM_COUNT)))
        # The rule of cycle A only shows where its legacy rebuild differs from what it built, i.e. where its quarter
        # is short or empty. With 3 masternodes the previous quarters of an index, as rebuilt by the end of
        # test_legacy, usually hold all of them, which leaves cycle A nothing to build.
        assert self.quarter_a != dict(enumerate(self.built_quarters(activation))), self.quarter_a
        # a short quarter is where the legacy rebuild would have padded
        assert_greater_than(self.short_rebuilds, 0)

    def check_faithful(self, activation, cycle, index, members):
        self.check_quarters(activation, cycle, index, members)
        assert_equal(len(set(members)), len(members))

    def check_short_quorums(self, quorums):
        for quorum in quorums.values():
            if quorum is None:
                continue
            members = [m["proTxHash"] for m in quorum["members"]]
            assert_equal(len(set(members)), len(members))
            if len(members) < LLMQ_SIZE:
                self.short_quorums += 1

    def test_short_quorums(self, activation):
        self.log.info("From A+96 on no quorum repeats a member and short quorums form")
        oracle = partial(self.check_faithful, activation)
        self.short_quorums = 0
        for cycle in sorted(self.compositions):
            if cycle >= activation + 4 * CYCLE:
                for i in range(QUORUM_COUNT):
                    self.check_short_quorums({i: self.mined_quorum(self.nodes[0].getblockhash(cycle + i))})
        self.check_short_quorums(self.mine_rotation_cycle("faithful", on_composition=oracle))
        assert_greater_than(self.short_quorums, 0)

    def test_below_minimum(self, activation):
        self.log.info("A post-activation quorum below minSize does not form")
        oracle = partial(self.check_faithful, activation)
        node = self.nodes[0]
        # as feature_llmq_simplepose.py: isolate a masternode until it is PoSe-banned
        mn = self.mninfo[2]
        mn.get_node(self).setnetworkactive(False)
        self.wait_until(lambda: mn.get_node(self).getconnectioncount() == 0)
        self.online.remove(mn)
        for _ in range(3):
            self.check_short_quorums(self.mine_rotation_cycle("faithful", on_composition=oracle))
            if check_banned(node, mn):
                break
        assert check_banned(node, mn)

        below_minimum = None
        for _ in range(6):
            self.check_short_quorums(self.mine_rotation_cycle("faithful", on_composition=oracle))
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

    def check_recomputed_quarters(self, activation, quarter_entries):
        """Every post-activation cycle's quarters were computed again since the first quarter_entries log
        entries, and every computation of a cycle agreed with the earlier ones (cycle_quarters checks)"""
        recomputed = {int(QUARTER_COMPOSITION.search(e).group(1)) for e in self.quarter_scanner.scan()[quarter_entries:]}
        post_activation = {cycle for cycle in self.compositions if cycle > activation}
        assert post_activation <= recomputed, post_activation - recomputed
        for cycle in self.compositions:
            self.cycle_quarters(cycle)
        for cycle in range(activation + CYCLE, activation + 4 * CYCLE, CYCLE):
            for i in range(QUORUM_COUNT):
                assert_equal(self.cycle_quarters(cycle)[i][3 - (cycle - activation) // CYCLE], self.quarter_a[i])

    def reorg_activation(self, activation):
        """Disconnect everything from the first active block on and connect it again"""
        nodes = self.all_online_nodes()
        tip = self.nodes[0].getbestblockhash()
        first_active = self.nodes[0].getblockhash(activation + 1)
        for n in nodes:
            n.invalidateblock(first_active)
        for n in nodes:
            assert_equal(n.getblockcount(), activation)
        assert_equal(get_bip9_status(self.nodes[0]), "locked_in")
        for n in nodes:
            n.reconsiderblock(first_active)
        self.wait_until(lambda: all(n.getbestblockhash() == tip for n in nodes))
        assert_equal(self.quarter_steps_bip9(activation + 1)["status"], "active")

    def test_reorg(self, activation):
        self.log.info("A reorg across the activation keeps every member list")
        quarter_entries = len(self.quarter_scanner.scan())
        self.reorg_activation(activation)
        self.check_recorded_quorums()
        self.check_recomputed_quarters(activation, quarter_entries)

    def test_restart(self, activation):
        self.log.info("A restart (cold caches) keeps every member list")
        quarter_entries = len(self.quarter_scanner.scan())
        self.restart_node(0, extra_args=self.extra_args[0])
        force_finish_mnsync(self.nodes[0])
        self.restart_masternodes(mns=self.online)
        # 'quorum info' rebuilds the recorded quorums from the stored snapshots
        self.check_recorded_quorums()
        # quorums that never formed are only rebuilt when their cycle is disconnected and connected again
        self.reorg_activation(activation)
        self.check_recorded_quorums()
        self.check_recomputed_quarters(activation, quarter_entries)


def get_bip9_status(node):
    return node.getdeploymentinfo()["deployments"]["quarter_steps"]["bip9"]["status"]


if __name__ == '__main__':
    LLMQRotationShortQuartersTest().main()
