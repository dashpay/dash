#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import json
import math
import statistics
from dataclasses import asdict, dataclass, field
from typing import Any, Dict, List, Optional


def _percentile(data: List[float], p: float) -> float:
    """Return the *p*-th percentile (0–100) of a **sorted** list."""
    if not data:
        return 0.0
    k = (len(data) - 1) * p / 100.0
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return data[int(k)]
    return data[f] * (c - k) + data[c] * (k - f)


@dataclass
class BenchResult:
    """Statistics computed from a set of latency samples."""

    name: str
    sample_count: int = 0
    mean_ms: float = 0.0
    stddev_ms: float = 0.0
    min_ms: float = 0.0
    p50_ms: float = 0.0
    p90_ms: float = 0.0
    p99_ms: float = 0.0
    p999_ms: float = 0.0
    max_ms: float = 0.0
    total_ms: float = 0.0
    ops_per_sec: float = 0.0
    extra: Dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_samples(
        cls,
        name: str,
        samples_ms: List[float],
        extra: Optional[Dict[str, Any]] = None,
    ) -> "BenchResult":
        """Compute statistics from a list of latency values in ms."""
        r = cls(name=name)
        if not samples_ms:
            return r

        data = sorted(samples_ms)
        r.sample_count = len(data)
        r.total_ms = sum(data)
        r.mean_ms = round(r.total_ms / r.sample_count, 3)
        r.min_ms = round(data[0], 3)
        r.p50_ms = round(_percentile(data, 50), 3)
        r.p90_ms = round(_percentile(data, 90), 3)
        r.p99_ms = round(_percentile(data, 99), 3)
        r.p999_ms = round(_percentile(data, 99.9), 3)
        r.max_ms = round(data[-1], 3)
        r.total_ms = round(r.total_ms, 3)
        if r.sample_count > 1:
            r.stddev_ms = round(statistics.stdev(data), 3)
        if r.total_ms > 0:
            r.ops_per_sec = round(r.sample_count / (r.total_ms / 1000.0), 1)
        if extra:
            r.extra = extra
        return r

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> "BenchResult":
        return cls(**d)


def save_results(
    results: List[BenchResult],
    path: str,
    label: str = "",
    metadata: Optional[Dict[str, Any]] = None,
) -> None:
    """Write results to a JSON file."""
    data: Dict[str, Any] = {
        "label": label,
        "results": [r.to_dict() for r in results],
    }
    if metadata:
        data["metadata"] = metadata
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)

