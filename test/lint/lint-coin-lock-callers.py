#!/usr/bin/env python3
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Check that nothing starts calling the raw coin-lock primitives unnoticed.
#
# CWallet::LockCoin()/UnlockCoin() change only the lock. The deliberate-unlock
# variants, LockCoinByUser()/UnlockCoinByUser(), also record that the user made
# the decision, so that the automatic masternode-collateral and dust locks leave
# the outpoint alone. Picking the wrong one is silent: a user path on the raw
# primitives loses the decision on the next wallet load, and an internal hold on
# the ByUser variants leaves a live collateral unprotected.
#
# A new caller therefore has to be a deliberate choice. Add it below with a note
# saying which kind it is.
#
# Limits worth knowing: tests are skipped, because they legitimately drive the raw
# primitives to set a fixture up, so a test that means to model a user action has to
# pick the right one on its own. A file already on the list is trusted for any further
# raw call it grows. Untracked files are not scanned, since this walks `git ls-files`.

import re
import sys

from subprocess import check_output

# file -> why its calls use the raw primitives
ALLOWED = {
    "src/coinjoin/client.cpp": "mixing holds its own session inputs, never user intent",
    "src/wallet/interfaces.cpp": "lockCoin()/unlockCoin() keep their upstream meaning; "
                                 "the GUI's user actions call the ByUser variants",
    "src/wallet/rpc/spend.cpp": "locks the coins a just-created transaction spends",
    "src/wallet/spend.cpp": "locks the coins a just-created transaction spends",
    "src/wallet/walletdb.cpp": "replays persisted locks while loading",
    "src/wallet/wallet.cpp": "defines the primitives and the automatic protections",
    "src/wallet/wallet.h": "declares the primitives",
    "src/interfaces/wallet.h": "declares both the plain and the ByUser interface methods",
    "src/evo/providertx_service.cpp": "releases the transient hold CollateralLockGuard takes",
    "src/qt/masternodewizard.cpp": "releases the transient hold taken while preparing a registration",
}

TEST_PREFIXES = ("src/test/", "src/wallet/test/", "src/qt/test/")

# Matches both surfaces: CWallet::(Un)LockCoin() and interfaces::Wallet::(un)lockCoin().
# The plural bulk helpers, (un)lockCoins(), are Dash-specific user paths and do not match.
# Only member calls, so that a class of its own with a lockCoin() slot does not match.
CALL = re.compile(r"[.>]\s*(?:un|Un)?[lL]ockCoin\s*\(")


def main():
    files = check_output(["git", "ls-files", "--", "src/*.cpp", "src/*.h"], text=True, encoding="utf8").splitlines()
    unexpected = []
    for path in files:
        if path in ALLOWED or path.startswith(TEST_PREFIXES):
            continue
        try:
            with open(path, "r", encoding="utf-8") as handle:
                content = handle.read()
        except (OSError, UnicodeDecodeError):
            continue
        for number, line in enumerate(content.splitlines(), start=1):
            if CALL.search(line) and "ByUser" not in line:
                unexpected.append(f"{path}:{number}: {line.strip()}")

    if unexpected:
        print("New caller(s) of the raw coin-lock primitives:")
        print("\n".join(unexpected))
        print()
        print("Use LockCoinByUser()/UnlockCoinByUser() when the user asked for the lock "
              "change, so the automatic masternode-collateral and dust locks do not undo "
              "it. Otherwise add the file to ALLOWED in this script with a short reason.")
        sys.exit(1)


if __name__ == "__main__":
    main()
