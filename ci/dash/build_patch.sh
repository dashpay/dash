#!/usr/bin/env bash
# Copyright (c) 2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

set -eo pipefail

SH_NAME="$(basename "${0}")"

if [ -z "${BASE_DIR}" ]; then
  echo "${SH_NAME}: BASE_DIR not defined, cannot continue!";
  exit 1;
elif [ -z "${BUILD_TARGET}" ]; then
  echo "${SH_NAME}: BUILD_TARGET not defined, cannot continue!";
  exit 1;
elif  [ ! "$(command -v uname)" ]; then
  echo "${SH_NAME}: uname not found, cannot continue!";
  exit 1;
elif  [ ! "$(command -v patchelf)" ]; then
  echo "${SH_NAME}: patchelf not found, cannot continue!";
  exit 1;
elif [ ! -d "${BASE_DIR}/build-ci/dashcore-${BUILD_TARGET}/src" ]; then
  echo "${SH_NAME}: cannot find directory for binaries to patch, cannot continue!";
  exit 1;
fi

# uname -m | interpreter
# -------- | -----------------------------
# aarch64  | /lib/ld-linux-aarch64.so.1
# armhf    | /lib/ld-linux-armhf.so.3
# i686     | /lib/ld-linux.so.2
# riscv64  | /lib/ld-linux-riscv64-lp64d.so.1
# x86_64   | /lib64/ld-linux-x86-64.so.2

INTERPRETER=""
case "${BUILD_TARGET}" in
  "linux64_nowallet" | "linux64_sqlite")
    INTERPRETER="/lib64/ld-linux-x86-64.so.2";
    ;;
  *)
    echo "${SH_NAME}: Nothing to do, exiting!";
    exit 0;
    ;;
esac

BINARIES=(
  "dashd"
  "dash-cli"
  "dash-gui"
  "dash-node"
  "dash-tx"
  "dash-wallet"
  "bench/bench_dash"
  "qt/dash-qt"
  "qt/test/test_dash-qt"
  "test/test_dash"
  "test/fuzz/fuzz"
)

for target in "${BINARIES[@]}"
do
  target_path="${BASE_DIR}/build-ci/dashcore-${BUILD_TARGET}/src/${target}"
  if [[ -f "${target_path}" ]]; then
    patchelf --set-interpreter "${INTERPRETER}" "${target_path}";
  fi
done
