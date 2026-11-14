#!/usr/bin/env python3
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Keep the legacy BLS scheme confined to the code that handles historical data.
#
# A BLS call that names no scheme is basic. Outside the wrappers in src/bls, the
# tests and the benchmarks, a line may name BLSScheme::Legacy only where a
# version or a block selects it, and says so with NOLINT(bls-legacy-scheme) on
# that line or NOLINTNEXTLINE(bls-legacy-scheme) on the line before. A marker
# that no longer guards such a line fails too.

import subprocess
import sys

LEGACY = "BLSScheme::Legacy"
SAME_LINE = "NOLINT(bls-legacy-scheme)"
NEXT_LINE = "NOLINTNEXTLINE(bls-legacy-scheme)"

EXCLUDED = [
    ":(exclude)src/bench",
    ":(exclude)src/bls",
    ":(exclude)src/qt/test",
    ":(exclude)src/test",
    ":(exclude)src/wallet/test",
]


def main():
    grep = subprocess.run(
        ["git", "grep", "-l", "-F", "-e", LEGACY, "-e", "(bls-legacy-scheme)", "--",
         "src/*.cpp", "src/*.h", *EXCLUDED],
        stdout=subprocess.PIPE, text=True, encoding="utf8", check=False,
    )
    if grep.returncode > 1:
        sys.exit(grep.returncode)
    files = grep.stdout.split()

    exit_code = 0
    for path in files:
        with open(path, encoding="utf8") as source:
            lines = source.read().splitlines()
        for number, line in enumerate(lines, start=1):
            names_legacy = LEGACY in line
            after_marker = number > 1 and NEXT_LINE in lines[number - 2]
            if names_legacy and SAME_LINE not in line and not after_marker:
                print(f"{path}:{number} names {LEGACY}. Derive the scheme from a version or from the block being "
                      f"validated instead, or mark the line with // {SAME_LINE} if legacy is needed there.")
                exit_code = 1
            if SAME_LINE in line and not names_legacy:
                print(f"{path}:{number} is marked {SAME_LINE} but does not name {LEGACY}. Remove the marker.")
                exit_code = 1
            if NEXT_LINE in line and (number == len(lines) or LEGACY not in lines[number]):
                print(f"{path}:{number} is marked {NEXT_LINE} but the next line does not name {LEGACY}. "
                      f"Remove the marker.")
                exit_code = 1
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
