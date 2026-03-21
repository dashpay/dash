#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import asyncio
import urllib.parse
from typing import Any, Dict, List, Tuple

from bench_framework import BenchFramework
from bench_helpers import async_rest_discover, async_rest_flood
from bench_results import BenchResult


class RestBench(BenchFramework):

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
            help="Max concurrent connections for storm/keepalive tests (default: 100)",
        )
        parser.add_argument(
            "--scale-max",
            dest="scale_max",
            type=int,
            default=2000,
            help="Max concurrency level for the scaling test (default: 2000)",
        )
        parser.add_argument(
            "--connect-burst",
            dest="connect_burst",
            type=int,
            default=1024,
            help="Max simultaneous initial TCP connections during scaling test (default: 1024)",
        )

    def set_bench_params(self) -> None:
        self._concurrency: int = 100
        self._duration: float = 10.0
        self.bench_iterations = 1
        self.bench_name = "rest_throughput"
        self.extra_args = [["-rest"]]
        self.num_nodes = 1
        self.setup_clean_chain = False
        self.warmup_iterations = 0

    def run_bench(self) -> None:
        self._duration = self.options.bench_duration
        self._concurrency = self.options.concurrency
        self._scale_max: int = self.options.scale_max
        self._connect_burst: int = self.options.connect_burst
        parsed = urllib.parse.urlparse(self.nodes[0].url)
        host = parsed.hostname
        port = parsed.port

        self.log.info("Discovering REST endpoints on %s:%d...", host, port)
        endpoints = asyncio.run(self._discover_endpoints(host, port))
        if not endpoints.get("light"):
            self.log.error("No responsive REST endpoint found")
            return

        light_path = endpoints["light"]
        heavy_path = endpoints.get("heavy")
        self.log.info("  light: %s", light_path)
        if heavy_path:
            self.log.info("  heavy: %s", heavy_path)

        # Connection storm
        self.log.info("[1/4] Connection storm (c=%d, %ds)...", self._concurrency, self._duration)
        result = asyncio.run(
            async_rest_flood(host, port, light_path,
                             concurrency=self._concurrency,
                             duration_s=self._duration,
                             force_close=True)
        )
        for lat in result["latencies_ms"]:
            self.record_sample("conn_storm", lat)
        self._log_result("conn_storm", result)

        # Sustained keep-alive
        self.log.info("[2/4] Sustained keep-alive (c=%d, %ds)...", self._concurrency, self._duration)
        result = asyncio.run(
            async_rest_flood(host, port, light_path,
                             concurrency=self._concurrency,
                             duration_s=self._duration,
                             connect_burst=self._connect_burst)
        )
        for lat in result["latencies_ms"]:
            self.record_sample("keepalive", lat)
        self._log_result("keepalive", result)

        # Connection scaling
        levels: List[int] = [1, 10, 50, 100, 200, 500, 1000, 2000, 5000]
        levels = [c for c in levels if c <= self._scale_max]
        scale_duration = max(self._duration / 3, 3.0)
        self.log.info("[3/4] Connection scaling %s...", levels)
        for c in levels:
            result = asyncio.run(
                async_rest_flood(host, port, light_path,
                                 concurrency=c,
                                 duration_s=scale_duration,
                                 connect_burst=self._connect_burst)
            )
            for lat in result["latencies_ms"]:
                self.record_sample(f"scaling_c{c}", lat)
            br = BenchResult.from_samples(f"c={c}", result["latencies_ms"])
            err_rate = ""
            if result["failed"] > 0:
                total = result["success"] + result["failed"]
                pct = result["failed"] / total * 100 if total else 0
                err_rate = f"  err={pct:.1f}%"
            err_detail = ""
            if result["status_codes"]:
                err_detail = f"  status_codes={result['status_codes']}"
            self.log.info(
                "       c=%-5d  %8.1f ops/s  mean=%.2fms  p99=%.2fms%s%s",
                c, br.ops_per_sec, br.mean_ms, br.p99_ms, err_rate, err_detail,
            )

        # Mixed load
        if not heavy_path:
            self.log.info("[4/4] Mixed load — skipped (no heavy endpoint)")
        else:
            self.log.info("[4/4] Mixed load (light c=%d, heavy c=10, %ds)...", self._concurrency, self._duration)
            light_result, heavy_result = asyncio.run(
                self._mixed_load(host, port, light_path, heavy_path)
            )
            for lat in light_result["latencies_ms"]:
                self.record_sample("mixed_light", lat)
            for lat in heavy_result["latencies_ms"]:
                self.record_sample("mixed_heavy", lat)
            self._log_result("mixed_light", light_result)
            self._log_result("mixed_heavy", heavy_result)

    async def _discover_endpoints(
        self, host: str, port: int,
    ) -> Dict[str, Any]:
        """Discover available REST endpoints and return a dict of paths."""
        light_path, heavy_path = await async_rest_discover(host, port)
        result: Dict[str, Any] = {}
        if light_path:
            result["light"] = light_path
        if heavy_path:
            result["heavy"] = heavy_path
        return result

    async def _mixed_load(
        self, host: str, port: int, light_path: str, heavy_path: str,
    ) -> Tuple[Dict[str, Any], Dict[str, Any]]:
        """Run light and heavy requests concurrently."""
        light_task = asyncio.create_task(
            async_rest_flood(host, port, light_path,
                             concurrency=self._concurrency,
                             duration_s=self._duration)
        )
        heavy_task = asyncio.create_task(
            async_rest_flood(host, port, heavy_path,
                             concurrency=10,
                             duration_s=self._duration)
        )
        return await light_task, await heavy_task

    def _log_result(self, name: str, result: Dict[str, Any]) -> None:
        br = BenchResult.from_samples(name, result["latencies_ms"])
        err_str = ""
        if result["failed"] > 0:
            err_str = f"  errors={result['failed']}"
        self.log.info(
            "       %8.1f ops/s  mean=%.2fms  p99=%.2fms%s",
            br.ops_per_sec, br.mean_ms, br.p99_ms, err_str,
        )


if __name__ == '__main__':
    RestBench().main()
