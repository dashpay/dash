#!/usr/bin/env bash
#
# Copyright (c) 2019-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_qt5
export PACKAGES="python3-zmq qtbase5-dev qttools5-dev-tools libdbus-1-dev libharfbuzz-dev"
export DEP_OPTS=""
# Run extended tests so that coverage does not fail, but exclude the very slow dbcrash
# On native ARM (aarch64), v0.12.1.5 has no aarch64 binary; exclude the test
# that requires it and limit previous release downloads to available versions
if [ "$(uname -m)" = "aarch64" ]; then
  export TEST_RUNNER_EXTRA="--previous-releases --coverage --extended --exclude feature_pruning,feature_dbcrash,feature_unsupported_utxo_db"
  export PREVIOUS_RELEASES_TAGS="v21.1.1 v20.1.1 v19.3.0 v18.2.2 v0.17.0.3 v0.16.1.1 v0.15.0.0"
else
  export TEST_RUNNER_EXTRA="--previous-releases --coverage --extended --exclude feature_pruning,feature_dbcrash"
fi
export RUN_UNIT_TESTS_SEQUENTIAL="true"
export RUN_UNIT_TESTS="false"
export GOAL="install"
export DOWNLOAD_PREVIOUS_RELEASES="true"
export BITCOIN_CONFIG="--enable-zmq --with-libs=no --enable-reduce-exports LDFLAGS=-static-libstdc++"
