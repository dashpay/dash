# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_library(bitcoin_util STATIC EXCLUDE_FROM_ALL
  asmap.cpp
  bip32.cpp
  bytevectorhash.cpp
  check.cpp
  edge.cpp
  error.cpp
  fees.cpp
  fs.cpp
  fs_helpers.cpp
  getuniquepath.cpp
  hasher.cpp
  message.cpp
  moneystr.cpp
  readwritefile.cpp
  ranges_set.cpp
  serfloat.cpp
  settings.cpp
  sock.cpp
  spanparsing.cpp
  strencodings.cpp
  string.cpp
  system.cpp
  syserror.cpp
  thread.cpp
  threadinterrupt.cpp
  threadnames.cpp
  time.cpp
  tokenpipe.cpp
  wpipe.cpp
  ../bls/bls_ies.cpp
  ../bls/bls_worker.cpp
  ../coinjoin/common.cpp
  ../coinjoin/options.cpp
  ../interfaces/echo.cpp
  ../interfaces/handler.cpp
  ../interfaces/init.cpp
  ../logging.cpp
  ../messagesigner.cpp
  ../random.cpp
  ../randomenv.cpp
  ../stacktraces.cpp
  ../support/cleanse.cpp
  ../support/lockedpool.cpp
  ../sync.cpp
)

target_link_libraries(bitcoin_util
  PRIVATE
    core_interface
    bitcoin_clientversion
    bitcoin_crypto
    Boost::headers
    dashbls
    univalue
    $<$<PLATFORM_ID:Windows>:ws2_32>
)
