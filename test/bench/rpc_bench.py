#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import asyncio
from typing import List

from bench_framework import BenchFramework
from bench_helpers import async_rpc_flood
from bench_results import BenchResult


class RpcBench(BenchFramework):

    def set_test_params(self) -> None:
        super().set_test_params()

    def run_test(self) -> None:
        super().run_test()

    def add_options(self, parser) -> None:  # type: ignore[override]
        super().add_options(parser)
        parser.add_argument(
            "--duration",
            dest="bench_duration",
            type=float,
            default=10.0,
            help="Duration per test in seconds (default: 10)",
        )
        parser.add_argument(
            "--concurrency",
            dest="concurrency",
            type=int,
            default=100,
            help="Max concurrent connections (default: 100)",
        )

    def set_bench_params(self) -> None:
        self._concurrency: int = 100
        self._duration: float = 10.0
        self.bench_iterations = 1
        self.bench_name = "rpc_throughput"
        self.num_nodes = 1
        self.setup_clean_chain = False
        self.warmup_iterations = 0

    def run_bench(self) -> None:
        self._duration = self.options.bench_duration
        self._concurrency = self.options.concurrency
        node = self.nodes[0]

        # Sequential baseline
        self.log.info("[1/3] Sequential baseline (c=1, %ds)...", self._duration)
        result = asyncio.run(
            async_rpc_flood(node, "getblockcount", concurrency=1,
                            duration_s=self._duration)
        )
        for lat in result["latencies_ms"]:
            self.record_sample("sequential", lat)
        self.log.info(
            "       %d requests, %d ok, %d failed",
            result["success"] + result["failed"],
            result["success"], result["failed"],
        )

        # Sustained keep-alive
        self.log.info(
            "[2/3] Sustained keep-alive (c=%d, %ds)...",
            self._concurrency, self._duration,
        )
        result = asyncio.run(
            async_rpc_flood(node, "getblockcount",
                            concurrency=self._concurrency,
                            duration_s=self._duration)
        )
        for lat in result["latencies_ms"]:
            self.record_sample("keepalive", lat)
        self.log.info(
            "       %d requests, %d ok, %d failed",
            result["success"] + result["failed"],
            result["success"], result["failed"],
        )

        # Connection scaling
        levels: List[int] = [1, 10, 50, 100, 200]
        levels = [c for c in levels if c <= self._concurrency * 2]
        scale_duration = max(self._duration / 3, 3.0)
        self.log.info("[3/3] Connection scaling %s...", levels)
        for c in levels:
            result = asyncio.run(
                async_rpc_flood(node, "getblockcount",
                                concurrency=c,
                                duration_s=scale_duration)
            )
            for lat in result["latencies_ms"]:
                self.record_sample(f"scaling_c{c}", lat)
            br = BenchResult.from_samples(f"c={c}", result["latencies_ms"])
            self.log.info(
                "       c=%-5d  %8.1f ops/s  mean=%.2fms  p99=%.2fms",
                c, br.ops_per_sec, br.mean_ms, br.p99_ms,
            )


if __name__ == '__main__':
    RpcBench().main()
