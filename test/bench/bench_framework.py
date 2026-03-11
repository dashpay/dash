#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import os
import sys
import time
from typing import Dict, List, Optional

# Allow imports from the functional test framework.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'functional'))

from bench_results import (  # noqa: E402
    BenchResult,
    results_to_markdown,
    save_results,
)
from test_framework.test_framework import BitcoinTestFramework  # noqa: E402


class BenchFramework(BitcoinTestFramework):

    def set_test_params(self) -> None:
        """Initialise benchmark state, then delegate to ``set_bench_params``."""
        self.warmup_iterations: int = 0
        self.bench_iterations: int = 1
        self.bench_name: str = type(self).__name__
        self.results_file: Optional[str] = None
        # Raw latency samples keyed by measurement name.
        self._samples: Dict[str, List[float]] = {}
        self._timer_start: Optional[float] = None
        self.set_bench_params()

    def setup_nodes(self) -> None:
        """Merge --daemon-args into extra_args just before nodes start."""
        raw = getattr(self.options, "daemon_args", None) or ""
        daemon_args = raw.split() if raw else []
        if daemon_args and self.extra_args is not None:
            for node_args in self.extra_args:
                node_args.extend(daemon_args)
        super().setup_nodes()

    def run_test(self) -> None:
        """Execute warmup, timed iterations, then report."""
        self.results_file = getattr(self.options, "results_file", None)
        if self.warmup_iterations > 0:
            self.log.info(
                "Warming up (%d iteration%s)...",
                self.warmup_iterations,
                "s" if self.warmup_iterations != 1 else "",
            )
            for i in range(self.warmup_iterations):
                self.log.debug("  warmup %d/%d", i + 1, self.warmup_iterations)
                self.run_bench()
                self._samples.clear()

        self.log.info(
            "Running benchmark (%d iteration%s)...",
            self.bench_iterations,
            "s" if self.bench_iterations != 1 else "",
        )
        for i in range(self.bench_iterations):
            self.log.debug("  iteration %d/%d", i + 1, self.bench_iterations)
            self.run_bench()

        self._report_results()

    def add_options(self, parser) -> None:  # type: ignore[override]
        """Adds bench-specific args. Subclasses should call super first."""
        parser.add_argument(
            "--daemon-args",
            dest="daemon_args",
            default=None,
            help="Extra daemon arguments as a single string "
                 "(e.g. --daemon-args=\"-rpcworkqueue=1024 -rpcthreads=8\")",
        )
        parser.add_argument(
            "--results-file",
            dest="results_file",
            default=None,
            help="Save results to a JSON file",
        )

    def set_bench_params(self) -> None:
        """Benchmarks must override this to set ``num_nodes``, etc."""
        raise NotImplementedError

    def run_bench(self) -> None:
        """Benchmarks must override this to define the workload."""
        raise NotImplementedError

    def start_timer(self) -> None:
        """Mark the beginning of a timed section."""
        if self._timer_start is not None:
            self.log.warning("start_timer() called twice without stop_timer()")
        self._timer_start = time.perf_counter()

    def stop_timer(self, name: str) -> float:
        """Record elapsed time (ms) since the last ``start_timer()`` call, returns in ms."""
        if self._timer_start is None:
            raise RuntimeError("stop_timer() called without start_timer()")
        elapsed_ms = (time.perf_counter() - self._timer_start) * 1000.0
        self._timer_start = None
        self._samples.setdefault(name, []).append(elapsed_ms)
        return elapsed_ms

    def record_sample(self, name: str, value_ms: float) -> None:
        """Directly record a latency sample (ms) without using the timer."""
        self._samples.setdefault(name, []).append(value_ms)

    def _build_results(self) -> List[BenchResult]:
        """Convert raw samples into a list of ``BenchResult`` objects."""
        return [
            BenchResult.from_samples(name, samples)
            for name, samples in self._samples.items()
            if samples
        ]

    def _report_results(self) -> None:
        """Print a summary of all recorded measurements."""
        results = self._build_results()
        md = results_to_markdown(results, title=self.bench_name)
        print(md)

        if self.results_file:
            save_results(results, self.results_file, label=self.bench_name)
            self.log.info("Results saved to %s", self.results_file)

    @property
    def samples(self) -> Dict[str, List[float]]:
        """Access the raw sample data."""
        return self._samples
