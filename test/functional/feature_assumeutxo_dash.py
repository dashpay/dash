#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Check the Dash evo section dumptxoutset appends to a UTXO snapshot.

On a chain with rotating quorums, the section carries the rotation quarters
the planner derives from the carried commitments and the base's cycle, the
work blocks of the next cycle when they are already mined, the credit-pool
window and the header-proven base coinbase. Its hash is the single SHA256 of
the section bytes in the file.
"""

import hashlib
from pathlib import Path

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal

EVO_MARKER = b"DASHEVO\x00"
LLMQ_TEST_DIP0024 = 103
CYCLE = 24
WORK_DIFF_DEPTH = 8
CREDIT_POOL_PERIOD = 100  # regtest


class AssumeutxoDashTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(5, 4)
        self.set_dash_llmq_test_params(4, 4)

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        first = self.cycle_of(self.mine_cycle_quorum()[0]["height"])
        second = self.cycle_of(self.mine_cycle_quorum()[0]["height"])
        assert_equal(second, first + CYCLE)

        self.log.info("Dump at a base before the mining window of its own cycle")
        h = second + CYCLE
        self.advance_to(h + 3)
        evo = self.dump("before-window.dat")
        rotating = self.quorum_type(evo, LLMQ_TEST_DIP0024)
        assert_equal(rotating["rotation"], True)
        assert_equal(rotating["active_commitments"], 2)
        assert_equal(rotating["safety_commitments"], 2)
        base = h + 3
        # A_T: the carried cycles, the base's cycle h and the next cycle N.
        expected = self.quarters({first, second, h, h + CYCLE}, base)
        assert_equal(set(rotating["rotation_cycles"]), expected)
        assert_equal(expected, {first - 3 * CYCLE, first - 2 * CYCLE, first - CYCLE, first, second, h})
        # Every cycle up to h has its quarter carried, and N's work block is
        # not mined yet.
        assert_equal(rotating["bootstrap_work"], [])

        self.log.info("Dump at the block before the next cycle")
        self.advance_to(h + CYCLE - 1)
        evo = self.dump("before-next-cycle.dat")
        rotating = self.quorum_type(evo, LLMQ_TEST_DIP0024)
        # N's work block is mined but no quarter or commitment references it.
        assert h + CYCLE - WORK_DIFF_DEPTH <= h + CYCLE - 1
        assert_equal(rotating["bootstrap_work"], [h + CYCLE])

    def cycle_of(self, height):
        return height - height % CYCLE

    def advance_to(self, height):
        node = self.nodes[0]
        assert node.getblockcount() < height
        self.move_blocks(self.nodes, height - 1 - node.getblockcount())
        # The base certifies a recent ChainLock, so its ChainLock scans are
        # provable from the carried commitments.
        self.wait_for_chainlocked_block_all_nodes(node.getbestblockhash())
        self.move_blocks(self.nodes, 1)
        assert_equal(node.getblockcount(), height)

    def quarters(self, cycles, base):
        """S_T: the three quarters before each cycle that exist at the base."""
        return {c - k * CYCLE for c in cycles for k in (1, 2, 3) if CYCLE <= c - k * CYCLE <= base}

    def quorum_type(self, evo, llmq_type):
        matches = [q for q in evo["quorums"] if q["llmq_type"] == llmq_type]
        assert_equal(len(matches), 1)
        return matches[0]

    def dump(self, filename):
        node = self.nodes[0]
        result = node.dumptxoutset(filename)
        evo = result["evo"]
        assert_equal(result["base_height"], node.getblockcount())
        assert_equal(evo["version"], 4)
        assert_equal(evo["mn_count"], self.mn_count)
        assert_equal(evo["credit_history"], CREDIT_POOL_PERIOD)
        assert_equal(evo["base_coinbase"], True)
        assert evo["quorum_modifiers"] > 0
        assert evo["historical_mn_lists"] > 0

        # The section ends the file and its hash covers exactly its bytes.
        data = (Path(node.datadir) / self.chain / filename).read_bytes()
        start = data.find(EVO_MARKER)
        assert start > 0
        digest = hashlib.sha256(data[start + len(EVO_MARKER):]).digest()
        assert_equal(digest[::-1].hex(), evo["hash"])

        # Another node at the same tip, which derived its state itself, dumps
        # the same section.
        self.sync_blocks()
        assert_equal(self.nodes[1].dumptxoutset(filename)["evo"]["hash"], evo["hash"])
        return evo


if __name__ == "__main__":
    AssumeutxoDashTest().main()
