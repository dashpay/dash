#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import time
from typing import List

from bench_framework import BenchFramework
from bench_helpers import create_self_transfer_batch

from test_framework.authproxy import JSONRPCException  # noqa: E402
from test_framework.wallet import MiniWallet  # noqa: E402


class MempoolBench(BenchFramework):

    def set_test_params(self) -> None:
        super().set_test_params()

    def run_test(self) -> None:
        super().run_test()

    def add_options(self, parser) -> None:  # type: ignore[override]
        super().add_options(parser)
        parser.add_argument(
            "--tx-count",
            dest="tx_count",
            type=int,
            default=50,
            help="Transactions per iteration (default: 50)",
        )

    def set_bench_params(self) -> None:
        self._tx_count = 50
        self.bench_iterations = 3
        self.bench_name = "mempool_acceptance"
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.warmup_iterations = 1

    def run_bench(self) -> None:
        self._tx_count = self.options.tx_count
        node = self.nodes[0]
        wallet = MiniWallet(node)

        num_blocks = self._tx_count + 110
        self.log.info("Mining %d blocks for funding...", num_blocks)
        self.generate(wallet, num_blocks, sync_fun=self.no_op)

        # Pre-create transactions
        self.log.info("Creating %d transactions...", self._tx_count)
        tx_hexes: List[str] = create_self_transfer_batch(wallet, self._tx_count)

        # Submit and time each one
        self.log.info("Submitting %d transactions...", len(tx_hexes))
        accepted = 0
        rejected = 0
        t_batch_start = time.perf_counter()
        for tx_hex in tx_hexes:
            self.start_timer()
            try:
                node.sendrawtransaction(tx_hex)
                self.stop_timer("sendrawtransaction")
                accepted += 1
            except JSONRPCException:
                self.stop_timer("sendrawtransaction_rejected")
                rejected += 1

        batch_elapsed_ms = (time.perf_counter() - t_batch_start) * 1000.0
        self.record_sample("batch_total", batch_elapsed_ms)

        self.log.info(
            "Submitted %d tx: %d accepted, %d rejected in %.1fms",
            len(tx_hexes), accepted, rejected, batch_elapsed_ms,
        )

        # Clear mempool for next iteration by mining
        self.generate(node, 1, sync_fun=self.no_op)


if __name__ == '__main__':
    MempoolBench().main()
