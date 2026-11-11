#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Regression tests for the lint cache's disk budget (no clang-tidy required)."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


SCRIPT = Path(__file__).with_name("cleanup-ctcache.py")
MIB = 1024 * 1024


class CleanupCTCacheTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cache = Path(self.temp.name)

    def write_entry(self, name, size, timestamp):
        path = self.cache / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x" * size)
        os.utime(path, (timestamp, timestamp))
        return path

    def size_kib(self):
        return int(subprocess.check_output(["du", "-sk", str(self.cache)], text=True, encoding="utf8").split()[0])

    def trim(self, limit):
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(self.cache), str(limit)],
            check=True, capture_output=True, text=True, encoding="utf8",
        ).stdout

    def test_under_budget_keeps_even_old_entries(self):
        old = self.write_entry("ab/old", MIB // 2, 1)
        before = self.size_kib()
        self.trim(1)
        self.assertTrue(old.exists())
        self.assertEqual(before, self.size_kib())

    def test_unequal_outputs_and_stats_trim_to_budget_oldest_first(self):
        # Removing 20% of these files only removes tiny stats, not enough bytes.
        now = int(time.time())
        stats = [self.write_entry(f"stats/{i:02x}", 4, now - 200 + i) for i in range(10)]
        large = self.write_entry("ab/old large output", 2 * MIB, now - 100)
        hot = self.write_entry("cd/recent output", MIB // 2, now)
        self.assertGreater(self.size_kib(), 1024)
        output = self.trim(1)
        self.assertLessEqual(self.size_kib(), 1024)
        self.assertTrue(hot.exists())
        self.assertFalse(large.exists())
        self.assertTrue(all(not path.exists() for path in stats))
        self.assertFalse((self.cache / "ab").exists())
        self.assertIn("Removed 11 oldest cache files", output)
        self.assertEqual(self.trim(1), "")

    def test_empty_cache_and_invalid_limit(self):
        self.trim(1)
        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(self.cache), "0"],
            capture_output=True, text=True, encoding="utf8",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be positive", result.stderr)


if __name__ == "__main__":
    unittest.main()
