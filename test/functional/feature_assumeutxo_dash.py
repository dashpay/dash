#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Exercise Dash evo emission by dumptxoutset (loading is added in M5)."""

from pathlib import Path

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal


class AssumeutxoDashTest(DashTestFramework):
    def set_test_params(self):
        # Keep rotation out of this minimal M4 emission test. The three enabled
        # non-rotated test types each need two active plus one safety quorum.
        args = [[
            "-testactivationheight=dip0024@999999",
            "-vbparams=testdummy:999999999999:999999999999",
        ] for _ in range(4)]
        self.set_dash_test_params(4, 3, extra_args=args, evo_count=3)
        self.set_dash_llmq_test_params(3, 2)

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        for _ in range(self.evo_count):
            self.dynamically_add_masternode(evo=True)

        # Each DKG cycle forms all enabled non-rotated test quorum types. Four
        # cycles cover llmq_test_platform's larger safety retention horizon.
        for _ in range(4):
            self.mine_quorum(llmq_type_name="llmq_test", llmq_type=100)

        node = self.nodes[0]
        info = node.getblockchaininfo()
        assert info["blocks"] >= 100  # DIP3, v19 and v20 are active in DashTestFramework.
        result = node.dumptxoutset("assumeutxo-dash.dat")
        assert_equal(result["base_height"], node.getblockcount())
        assert len(result["evo_hash"]) == 64
        assert result["evo_mn_count"] >= 3

        snapshot_path = Path(node.datadir) / self.chain / "assumeutxo-dash.dat"
        data = snapshot_path.read_bytes()
        assert b"DASHEVO\x00" in data


if __name__ == "__main__":
    AssumeutxoDashTest().main()
