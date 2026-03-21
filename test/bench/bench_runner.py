#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import argparse
import configparser
import os
import subprocess
import sys
import time
from glob import glob
from typing import List, Optional, Tuple

TEST_EXIT_SKIPPED = 77

DEFAULT, BOLD, GREEN, RED, SKIP = ("", ""), ("", ""), ("", ""), ("", ""), ("", "")
if os.name != "nt" and sys.stdout.isatty():
    DEFAULT = ("\033[0m", "\033[0m")
    BOLD = ("\033[0m", "\033[1m")
    GREEN = ("\033[0m", "\033[0;32m")
    RED = ("\033[0m", "\033[0;31m")
    SKIP = ("\033[0m", "\033[0;33m")

TICK, CROSS = "P ", "x"
try:
    "\u2713".encode("utf_8").decode(sys.stdout.encoding)
    TICK = "\u2713 "
    CROSS = "\u2716 "
except Exception:
    pass  # Do nothing


def _get_build_id() -> str:
    """Return daemon version string, or 'unknown' on failure."""
    config = configparser.ConfigParser()
    configfile = os.path.join(
        os.path.dirname(__file__), "..", "config.ini",
    )
    try:
        with open(configfile, encoding="utf8") as f:
            config.read_file(f)
    except (FileNotFoundError, configparser.Error):
        return "unknown"
    build_dir = config.get("environment", "BUILDDIR", fallback="")
    dashd = os.path.join(build_dir, "src", "dashd")
    if not os.path.isfile(dashd):
        return "unknown"
    try:
        out = subprocess.check_output(
            [dashd, "--version"], text=True, timeout=5,
        )
        # First line: "Dash Core version v23.1.0-167-gceab392..."
        first_line = out.strip().splitlines()[0]
        return first_line.replace("Dash Core version ", "")
    except (subprocess.SubprocessError, IndexError):
        return "unknown"


def discover_benchmarks(bench_dir: str) -> List[str]:
    """Return sorted list of benchmark script filenames."""
    pattern = os.path.join(bench_dir, "*_bench.py")
    all_files = glob(pattern)
    return sorted(
        os.path.basename(f) for f in all_files
        if not os.path.basename(f).startswith("bench_")
    )


def _extract_markdown(output: str) -> Optional[str]:
    """Extract Markdown table block from benchmark output."""
    lines = output.splitlines()
    md_lines: List[str] = []
    capturing = False

    for raw_line in lines:
        line = raw_line.rstrip()
        if line.startswith("## "):
            capturing = True
            md_lines.append(line)
        elif capturing:
            if line.startswith("|") or line == "":
                md_lines.append(line)
            else:
                capturing = False

    if not md_lines:
        return None
    # Strip trailing blank lines.
    while md_lines and md_lines[-1] == "":
        md_lines.pop()
    return "\n".join(md_lines)


def _extract_failure_log(output: str) -> str:
    """Extract failure information from benchmark output."""
    lines = output.splitlines()
    relevant: List[str] = []
    in_traceback = False

    for line in lines:
        if "Traceback (most recent call last)" in line:
            in_traceback = True
        if in_traceback:
            relevant.append(line)
            if relevant and not line.startswith(" ") and "Traceback" not in line:
                in_traceback = False
        elif "(ERROR)" in line:
            relevant.append(line)

    return "\n".join(relevant) if relevant else output


def run_benchmark(
    bench_dir: str,
    script: str,
    extra_args: List[str],
    timeout: int = 600,
) -> Tuple[int, float, str]:
    """Run a single benchmark script."""
    cmd = [sys.executable, os.path.join(bench_dir, script)] + extra_args
    t0 = time.time()
    try:
        result = subprocess.run(
            cmd,
            cwd=os.path.dirname(bench_dir) or ".",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        elapsed = time.time() - t0
        return result.returncode, elapsed, result.stdout or ""
    except subprocess.TimeoutExpired:
        elapsed = time.time() - t0
        return 1, elapsed, f"Benchmark timed out after {timeout}s"


def main() -> None:
    bench_dir = os.path.dirname(os.path.abspath(__file__))

    # Split on '--': runner args before, benchmark passthrough after.
    argv = sys.argv[1:]
    if "--" in argv:
        split = argv.index("--")
        runner_argv = argv[:split]
        passthrough = argv[split + 1:]
    else:
        runner_argv = argv
        passthrough = []

    parser = argparse.ArgumentParser(
        description="Run Dash Core benchmarks",
    )
    parser.add_argument(
        "benchmarks",
        nargs="*",
        help="Specific benchmark scripts to run (default: all)",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List available benchmarks and exit",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=600,
        help="Per-benchmark timeout in seconds (default: 600)",
    )
    args = parser.parse_args(runner_argv)

    available = discover_benchmarks(bench_dir)

    if args.list:
        print("Available benchmarks:")
        for name in available:
            print(f"  {name}")
        return

    to_run = args.benchmarks if args.benchmarks else available
    if not to_run:
        print("No benchmarks found.", file=sys.stderr)
        sys.exit(1)

    for name in to_run:
        if name not in available:
            print(f"Unknown benchmark: {name}", file=sys.stderr)
            print(f"Available: {', '.join(available)}", file=sys.stderr)
            sys.exit(1)

    build_id = _get_build_id()
    print(f"Running benchmarks for Dash Core {build_id}\n")

    total = len(to_run)
    passed = 0
    skipped = 0
    failed_names: List[str] = []
    markdown_blocks: List[str] = []

    for i, name in enumerate(to_run, 1):
        rc, elapsed, output = run_benchmark(
            bench_dir, name, passthrough, timeout=args.timeout,
        )
        duration = int(elapsed)
        label = f"{i}/{total} - {BOLD[1]}{name}{BOLD[0]}"

        if rc == 0:
            passed += 1
            print(f"{GREEN[1]}{TICK}{label} passed, Duration: {duration} s{GREEN[0]}")
            md = _extract_markdown(output)
            if md:
                markdown_blocks.append(md)
        elif rc == TEST_EXIT_SKIPPED:
            skipped += 1
            print(f"{SKIP[1]}{TICK}{label} skipped{SKIP[0]}")
        else:
            failed_names.append(name)
            print(f"{RED[1]}{CROSS}{label} failed, Duration: {duration} s{RED[0]}")
            print(f"\n{BOLD[1]}Error log:{BOLD[0]}")
            print(_extract_failure_log(output))
            print()

    failed = len(failed_names)
    print(
        f"\n{BOLD[1]}{passed} passed, {failed} failed, "
        f"{skipped} skipped{BOLD[0]}"
    )

    if markdown_blocks:
        print("\n---\n")
        print("\n\n".join(markdown_blocks))

    if failed_names:
        print(f"{RED[1]}Failed: {', '.join(failed_names)}{RED[0]}")

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
