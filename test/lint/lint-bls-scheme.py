#!/usr/bin/env python3
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Keep the legacy BLS scheme confined to the code that handles historical data.
#
# A BLS call that names no scheme is basic. BLSScheme::Legacy may only be named
# where a version or a block selects it, so each file that does so is listed
# here with its reason.

import subprocess
import sys

LEGACY_SCHEME_FILES = {
    # The wrappers themselves
    "src/bls/bls.cpp",
    "src/bls/bls.h",
    # Scheme of a block, of a provider transaction version and of a commitment version
    "src/deploymentstatus.h",
    "src/evo/types.h",
    "src/llmq/commitment.h",
    # Operator keys are unique per encoding, so both encodings are probed
    "src/evo/deterministicmns.h",
    "src/txmempool.cpp",
    # Masternode list diffs written before the diff carried a version
    "src/evo/deterministicmns.cpp",
    "src/evo/dmnstate.cpp",
    # Display and parsing of keys and signatures in either encoding
    "src/active/masternode.cpp",
    "src/rpc/evo.cpp",
    "src/rpc/quorums.cpp",
}

EXCLUDED = [
    ":(exclude)src/bench",
    ":(exclude)src/qt/test",
    ":(exclude)src/test",
    ":(exclude)src/wallet/test",
]


def main():
    found = set(subprocess.run(
        ["git", "grep", "-l", "BLSScheme::Legacy", "--", "src/*.cpp", "src/*.h", *EXCLUDED],
        stdout=subprocess.PIPE, text=True, encoding="utf8", check=False,
    ).stdout.split())

    exit_code = 0
    for path in sorted(found - LEGACY_SCHEME_FILES):
        print(f"{path} names BLSScheme::Legacy. Derive the scheme from a version or from the block being "
              f"validated instead, or list the file in {sys.argv[0]} with the reason legacy is needed there.")
        exit_code = 1
    for path in sorted(LEGACY_SCHEME_FILES - found):
        print(f"{path} no longer names BLSScheme::Legacy. Remove it from {sys.argv[0]}.")
        exit_code = 1
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
