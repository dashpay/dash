#!/usr/bin/env bash
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

# Depends with the Dash Platform CXX bindings; linux64_sqlite builds against it.
export CONTAINER_NAME=ci_native_platform_gui
export HOST=x86_64-pc-linux-gnu
export DEP_OPTS="PLATFORM_GUI=1"
