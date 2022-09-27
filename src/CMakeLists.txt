# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

set(CRC32C_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CRC32C_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CRC32C_USE_GLOG OFF CACHE BOOL "" FORCE)
set(CRC32C_INSTALL OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)
mark_as_advanced(
  CRC32C_BUILD_TESTS
  CRC32C_BUILD_BENCHMARKS
  CRC32C_USE_GLOG
  CRC32C_INSTALL
)
add_subdirectory(crc32c EXCLUDE_FROM_ALL)

set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEVELDB_INSTALL OFF CACHE BOOL "" FORCE)
set(HAVE_CRC32C ON CACHE BOOL "" FORCE)
set(HAVE_SNAPPY OFF CACHE BOOL "" FORCE)
set(HAVE_TCMALLOC OFF CACHE BOOL "" FORCE)
# Disabling TSA warnings because leveldb simultaneously
# enables -Werror as a usage requirement.
set(HAVE_CLANG_THREAD_SAFETY OFF CACHE BOOL "" FORCE)
mark_as_advanced(
  LEVELDB_BUILD_TESTS
  LEVELDB_BUILD_BENCHMARKS
  LEVELDB_INSTALL
  HAVE_CRC32C
  HAVE_SNAPPY
  HAVE_TCMALLOC
  HAVE_CLANG_THREAD_SAFETY
)
add_subdirectory(leveldb EXCLUDE_FROM_ALL)
target_compile_definitions(leveldb
  PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_NONSTDC_NO_WARNINGS>
)
target_compile_options(leveldb
  PRIVATE
    # RTTI has been forcibly disabled in CMake configuration since leveldb v1.23.
    # Commit: 69061b464ab1da287da9b7ffec1ed911b754403b
    # Using leveldb::Logger as a base class to derive CBitcoinLevelDBLogger
    # requires RTTI been enabled.
    $<$<CXX_COMPILER_ID:GNU,Clang>:-frtti>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/wd4722>
)

configure_file(../cmake/bitcoin-config.h.in config/bitcoin-config.h @ONLY)
include_directories(${CMAKE_BINARY_DIR}/src)
add_definitions(-DHAVE_CONFIG_H)

add_subdirectory(crypto)
add_subdirectory(univalue)
add_subdirectory(util)


# Stable, backwards-compatible consensus functionality
# also exposed as a shared library and/or a static one.
add_library(bitcoin_consensus OBJECT EXCLUDE_FROM_ALL "")
target_sources(bitcoin_consensus
  PRIVATE
    arith_uint256.cpp
    consensus/merkle.cpp
    consensus/tx_check.cpp
    hash.cpp
    primitives/block.cpp
    primitives/transaction.cpp
    pubkey.cpp
    script/interpreter.cpp
    script/script.cpp
    script/script_error.cpp
    uint256.cpp
    util/strencodings.cpp
)
target_include_directories(bitcoin_consensus
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(bitcoin_consensus
  PRIVATE
    bitcoin_crypto
    secp256k1
)


# Home for common functionality shared by different executables and libraries.
# Similar to `bitcoin_util` library, but higher-level.
add_library(bitcoin_common STATIC EXCLUDE_FROM_ALL "")
target_sources(bitcoin_common
  PRIVATE
    base58.cpp
    bech32.cpp
    chainparams.cpp
    coins.cpp
    compressor.cpp
    core_read.cpp
    core_write.cpp
    key.cpp
    key_io.cpp
    merkleblock.cpp
    netaddress.cpp
    netbase.cpp
    net_permissions.cpp
    policy/feerate.cpp
    policy/policy.cpp
    protocol.cpp
    psbt.cpp
    rpc/rawtransaction_util.cpp
    rpc/util.cpp
    scheduler.cpp
    script/descriptor.cpp
    script/sign.cpp
    script/signingprovider.cpp
    script/standard.cpp
    warnings.cpp
)
target_include_directories(bitcoin_common
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(bitcoin_common
  PRIVATE
    bitcoin_consensus
    bitcoin_util
    univalue
    secp256k1
    Boost::headers
)


if(ENABLE_WALLET)
  add_subdirectory(wallet)

  if(BUILD_WALLET_TOOL)
    add_executable(bitcoin-wallet "")
    target_sources(bitcoin-wallet
      PRIVATE
        wallet/wallettool.cpp
    )
    target_include_directories(bitcoin-wallet
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    )
    target_link_libraries(bitcoin-wallet
      PRIVATE
        bitcoin_wallet
        bitcoin_common
        bitcoin_consensus
        bitcoin_util
        Boost::headers
    )
  endif()
endif()


# P2P and RPC server functionality used by `bitcoind` and `bitcoin-qt` executables.
add_library(bitcoin_node STATIC EXCLUDE_FROM_ALL "")
target_sources(bitcoin_node
  PRIVATE
    addrdb.cpp
    addrman.cpp
    banman.cpp
    blockencodings.cpp
    blockfilter.cpp
    chain.cpp
    consensus/tx_verify.cpp
    dbwrapper.cpp
    flatfile.cpp
    httprpc.cpp
    httpserver.cpp
    index/base.cpp
    index/blockfilterindex.cpp
    index/txindex.cpp
    init.cpp
    mapport.cpp
    net.cpp
    net_processing.cpp
    node/coin.cpp
    node/context.cpp
    node/transaction.cpp
    noui.cpp
    policy/fees.cpp
    policy/settings.cpp
    pow.cpp
    rest.cpp
    rpc/blockchain.cpp
    rpc/mining.cpp
    rpc/net.cpp
    rpc/rawtransaction.cpp
    rpc/server.cpp
    script/sigcache.cpp
    shutdown.cpp
    timedata.cpp
    torcontrol.cpp
    txdb.cpp
    txmempool.cpp
    validation.cpp
    validationinterface.cpp
    versionbits.cpp
)
if(ENABLE_WALLET)
  target_sources(bitcoin_node PRIVATE wallet/init.cpp)
  target_link_libraries(bitcoin_node PRIVATE bitcoin_wallet)
  if(USE_BDB)
    target_link_libraries(bitcoin_node PRIVATE BerkeleyDB::BerkeleyDB)
  endif()
else()
  target_sources(bitcoin_node PRIVATE dummywallet.cpp)
endif()
target_include_directories(bitcoin_node
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(bitcoin_node
  PRIVATE
    bitcoin_common
    bitcoin_consensus
    minisketch
    leveldb
    univalue
    Boost::headers
    $<$<NOT:$<PLATFORM_ID:Windows>>:PkgConfig::libevent_pthreads>
)
if(TARGET NATPMP::NATPMP)
  target_link_libraries(bitcoin_node PRIVATE NATPMP::NATPMP)
endif()
if(TARGET MiniUPnPc::MiniUPnPc)
  target_link_libraries(bitcoin_node PRIVATE MiniUPnPc::MiniUPnPc)
endif()


# Bitcoin Core bitcoind
if(BUILD_DAEMON)
  add_executable(bitcoind "")
  target_sources(bitcoind
    PRIVATE
      bitcoind.cpp
  )
  target_link_libraries(bitcoind
    PRIVATE
      bitcoin_node
      $<$<BOOL:${ENABLE_WALLET}>:bitcoin_wallet>
  )
endif()


if(BUILD_CLI OR NOT WITH_GUI STREQUAL "no")
  add_library(rpc_client STATIC EXCLUDE_FROM_ALL "")
  target_sources(rpc_client PRIVATE rpc/client.cpp)
  target_include_directories(rpc_client
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  )
  target_link_libraries(rpc_client PUBLIC univalue)
endif()


# Bitcoin Core RPC client
if(BUILD_CLI)
  add_executable(bitcoin-cli "")
  target_sources(bitcoin-cli
    PRIVATE
      bitcoin-cli.cpp
      compat/stdin.cpp
  )
  target_link_libraries(bitcoin-cli
    PRIVATE
      bitcoin_util
      rpc_client
      PkgConfig::libevent
  )
endif()


if(BUILD_TX)
  add_executable(bitcoin-tx "")
  target_sources(bitcoin-tx
    PRIVATE
      bitcoin-tx.cpp
  )
  target_include_directories(bitcoin-tx
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  )
  target_link_libraries(bitcoin-tx
    PRIVATE
      bitcoin_common
      bitcoin_util
      univalue
  )
endif()


if(BUILD_UTIL)
  add_executable(bitcoin-util "")
  target_sources(bitcoin-util
    PRIVATE
  )
  target_include_directories(bitcoin-util
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  )
  target_link_libraries(bitcoin-util
    PRIVATE
      bitcoin_common
      bitcoin_consensus
      bitcoin_util
  )
endif()


if(BUILD_BITCOINCONSENSUS_LIB)
  add_subdirectory(script)
endif()

if(BUILD_BITCOINKERNEL_LIB)
  add_subdirectory(kernel)
endif()

if(BUILD_UTIL_CHAINSTATE)
  add_executable(bitcoin-chainstate "")
  target_sources(bitcoin-chainstate
    PRIVATE
      bitcoin-chainstate.cpp
  )
  target_include_directories(bitcoin-chainstate
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  )
  if(TARGET bitcoinkernel AND NOT WIN32)
    target_link_libraries(bitcoin-chainstate PRIVATE bitcoinkernel)
  elseif(TARGET bitcoinkernel_static)
    target_link_libraries(bitcoin-chainstate PRIVATE bitcoinkernel_static)
  endif()
  target_link_libraries(bitcoin-chainstate
    PRIVATE
      bitcoin_crypto
      leveldb
      univalue
      secp256k1
      Boost::headers
      $<$<PLATFORM_ID:Windows>:ws2_32>
  )
endif()


if(NOT WITH_GUI STREQUAL "no")
  add_subdirectory(qt)
endif()
