#!/usr/bin/env bash
# Copyright (c) 2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# This script runs the Dash-specific linters

export LC_ALL=C.UTF-8

set -e

source ./ci/dash/matrix.sh

# Check commit scripts for PRs
if [ "$PULL_REQUEST" != "false" ]; then
    test/lint/commit-script-check.sh "$COMMIT_RANGE"
fi

# Run linters if CHECK_DOC is set
if [ "$CHECK_DOC" = 1 ]; then
    # Run all linters; the subtree and documentation checks are disabled in the runner
    RUST_BACKTRACE=1 cargo run --manifest-path "./test/lint/test_runner/Cargo.toml"
fi
