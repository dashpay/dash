#!/usr/bin/env bash
#
# Copyright (c) 2024-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

# Inherit packages, depends, unit-test flags and BITCOIN_CONFIG from the
# main native qt5 target so the canary builds the same binaries the ARM
# lane does, just on x86.
source ./ci/test/00_setup_env_native_qt5.sh

# Base-only functional tests: the ARM lane already runs --extended, so we
# only need a fast smoke suite on x86 to catch wallet/core regressions
# that might depend on x86 behavior (alignment, SSE, glibc-on-x86 quirks).
# Drop --extended and --previous-releases; base tests don't need them.
# Drop --coverage too: the coverage check requires every RPC to be
# exercised (e.g. pruneblockchain needs feature_pruning), and those
# tests are extended-only — the ARM lane owns RPC coverage tracking.
export TEST_RUNNER_EXTRA="--exclude feature_pruning,feature_dbcrash"
export DOWNLOAD_PREVIOUS_RELEASES="false"
unset PREVIOUS_RELEASES_TAGS
