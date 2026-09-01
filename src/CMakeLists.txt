# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(GNUInstallDirs)
include(AddWindowsResources)

configure_file(${PROJECT_SOURCE_DIR}/cmake/bitcoin-config.h.in config/bitcoin-config.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/immer)

# TODO: After the transition from Autotools to CMake, the obj/ subdirectory
#       could be dropped as its only purpose was to separate a generated header
#       from source files.
add_custom_target(generate_build_info
  BYPRODUCTS ${PROJECT_BINARY_DIR}/src/obj/build.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/src/obj
  COMMAND ${CMAKE_COMMAND} -DBUILD_INFO_HEADER_PATH=${PROJECT_BINARY_DIR}/src/obj/build.h -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/script/GenerateBuildInfo.cmake
  COMMENT "Generating obj/build.h"
  VERBATIM
)
add_library(bitcoin_clientversion OBJECT EXCLUDE_FROM_ALL
  clientversion.cpp
)
target_compile_definitions(bitcoin_clientversion
  PRIVATE
    HAVE_BUILD_INFO
)
target_link_libraries(bitcoin_clientversion
  PRIVATE
    core_interface
)
add_dependencies(bitcoin_clientversion generate_build_info)

add_subdirectory(crypto)
add_subdirectory(univalue)
add_subdirectory(util)
if(WITH_MULTIPROCESS)
  add_subdirectory(ipc)
endif()

#=============================
# secp256k1 subtree
#=============================
message("")
message("Configuring secp256k1 subtree...")
set(SECP256K1_DISABLE_SHARED ON CACHE BOOL "" FORCE)
# The ECDH module is required by the wallet's Platform key provider
# (DashPay contact request encryption, wallet/platformkeys.cpp).
set(SECP256K1_ENABLE_MODULE_ECDH ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS ${BUILD_TESTS} CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
include(GetTargetInterface)
# -fsanitize and related flags apply to both C++ and C,
# so we can pass them down to libsecp256k1 as CFLAGS.
get_target_interface(core_sanitizer_cxx_flags "" sanitize_interface COMPILE_OPTIONS)
set(SECP256K1_LATE_CFLAGS ${core_sanitizer_cxx_flags} CACHE STRING "" FORCE)
unset(core_sanitizer_cxx_flags)
# We want to build libsecp256k1 with the most tested RelWithDebInfo configuration.
enable_language(C)
foreach(config IN LISTS CMAKE_BUILD_TYPE CMAKE_CONFIGURATION_TYPES)
  if(config STREQUAL "")
    continue()
  endif()
  string(TOUPPER "${config}" config)
  set(CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endforeach()
# If the CFLAGS environment variable is defined during building depends
# and configuring this build system, its content might be duplicated.
if(DEFINED ENV{CFLAGS})
  deduplicate_flags(CMAKE_C_FLAGS)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)
add_subdirectory(secp256k1)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
string(APPEND CMAKE_C_COMPILE_OBJECT " ${APPEND_CPPFLAGS} ${APPEND_CFLAGS}")

set(BUILD_BLS_JS_BINDINGS OFF CACHE STRING "" FORCE)
set(BUILD_BLS_PYTHON_BINDINGS OFF CACHE STRING "" FORCE)
set(BUILD_BLS_TESTS OFF CACHE STRING "" FORCE)
set(BUILD_BLS_BENCHMARKS OFF CACHE STRING "" FORCE)
add_subdirectory(dashbls EXCLUDE_FROM_ALL)
# The dashbls and relic headers are noisy (mostly -Wdocumentation). Autotools
# pulls them in with -isystem; do the same here so that they do not drown out
# our own warnings, or fail the build outright when WERROR is set.
get_target_property(dashbls_include_dirs dashbls INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(dashbls PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dashbls_include_dirs}"
)
unset(dashbls_include_dirs)

# Stable, backwards-compatible consensus functionality.
set(consensus_sources
  arith_uint256.cpp
  bls/bls.cpp
  consensus/merkle.cpp
  consensus/tx_check.cpp
  hash.cpp
  primitives/block.cpp
  primitives/transaction.cpp
  pubkey.cpp
  script/bitcoinconsensus.cpp
  script/interpreter.cpp
  script/script.cpp
  script/script_error.cpp
  uint256.cpp
  util/strencodings.cpp
  util/string.cpp
)
# Internal-only; the installable library below is the public dashconsensus.
add_library(bitcoin_consensus STATIC EXCLUDE_FROM_ALL ${consensus_sources})
target_link_libraries(bitcoin_consensus
  PRIVATE
    core_interface
    bitcoin_crypto
    Boost::headers
    dashbls
    secp256k1
)

# Dash still ships the public consensus library that upstream dropped in
# bitcoin#29648, so build and install it here too. It gets its own compilation
# of the consensus and crypto sources because the public library is built
# without the optimised SHA-256 backends; see libdashconsensus_la in
# src/Makefile.am. This has to stay below add_subdirectory(crypto), which is
# what sets crypto_base_sources and crypto_sph_sources in this scope.
if(BUILD_CONSENSUS_LIB)
  add_library(dashconsensus SHARED
    ${consensus_sources}
    ${crypto_base_sources}
    ${crypto_sph_sources}
  )
  target_compile_definitions(dashconsensus
    PRIVATE
      BUILD_BITCOIN_INTERNAL
      DISABLE_OPTIMIZED_SHA256
      SPH_SMALL_FOOTPRINT_CUBEHASH=1
      SPH_SMALL_FOOTPRINT_JH=1
  )
  target_link_libraries(dashconsensus
    PRIVATE
      core_interface
      Boost::headers
      dashbls
      secp256k1
  )
  # libdashconsensus_la carries no -version-info, so libtool builds it as
  # 0:0:0 and installs libdashconsensus.so.0.0.0 with an .so.0 soname. Match
  # that here, otherwise consumers linked against an Autotools install cannot
  # resolve DT_NEEDED against a CMake one.
  set_target_properties(dashconsensus PROPERTIES VERSION 0.0.0 SOVERSION 0)
  if(APPLE)
    # libtool derives the Mach-O versions as (current - age + 1), so 0:0:0
    # yields 1.0.0 where CMake would otherwise take them from the properties
    # above and emit 0.0.0. dyld refuses a library whose compatibility version
    # is below the one recorded in the consumer, so set them explicitly.
    set_target_properties(dashconsensus PROPERTIES
      MACHO_COMPATIBILITY_VERSION 1.0.0
      MACHO_CURRENT_VERSION 1.0.0
    )
  endif()
  install(TARGETS dashconsensus
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
  # Automake's include_HEADERS strips the directory, installing the header
  # flat as <includedir>/bitcoinconsensus.h, which is the layout the
  # pkg-config file's "Cflags: -I${includedir}" expects. Match it.
  install(FILES script/bitcoinconsensus.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
  set(prefix ${CMAKE_INSTALL_PREFIX})
  set(exec_prefix "\${prefix}")
  set(libdir "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
  set(includedir "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  configure_file(${PROJECT_SOURCE_DIR}/libdashconsensus.pc.in
    ${PROJECT_BINARY_DIR}/libdashconsensus.pc @ONLY
  )
  install(FILES ${PROJECT_BINARY_DIR}/libdashconsensus.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
  )
endif()

if(WITH_ZMQ)
  add_subdirectory(zmq)
endif()

# Home for common functionality shared by different executables and libraries.
# Similar to `bitcoin_util` library, but higher-level.
add_library(bitcoin_common STATIC EXCLUDE_FROM_ALL
  base58.cpp
  bech32.cpp
  chainparams.cpp
  chainparamsbase.cpp
  coins.cpp
  common/bloom.cpp
  common/init.cpp
  common/run_command.cpp
  common/url.cpp
  compressor.cpp
  core_read.cpp
  core_write.cpp
  deploymentinfo.cpp
  evo/core_write.cpp
  evo/netinfo.cpp
  evo/providertx_util.cpp
  external_signer.cpp
  governance/common.cpp
  init/common.cpp
  key.cpp
  key_io.cpp
  llmq/core_write.cpp
  merkleblock.cpp
  net_permissions.cpp
  net_types.cpp
  netaddress.cpp
  netbase.cpp
  outputtype.cpp
  policy/feerate.cpp
  policy/policy.cpp
  protocol.cpp
  psbt.cpp
  rpc/external_signer.cpp
  rpc/rawtransaction_util.cpp
  rpc/request.cpp
  rpc/util.cpp
  saltedhasher.cpp
  scheduler.cpp
  script/descriptor.cpp
  script/miniscript.cpp
  script/sign.cpp
  script/signingprovider.cpp
  script/standard.cpp
  warnings.cpp
)
target_link_libraries(bitcoin_common
  PRIVATE
    core_interface
    bitcoin_consensus
    bitcoin_util
    leveldb
    univalue
    dashbls
    secp256k1
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:libevent::libevent>
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
    $<$<PLATFORM_ID:Windows>:ws2_32>
)


set(installable_targets)
if(ENABLE_WALLET)
  add_subdirectory(wallet)

  if(BUILD_WALLET_TOOL)
    add_executable(bitcoin-wallet
      bitcoin-wallet.cpp
      init/bitcoin-wallet.cpp
      wallet/wallettool.cpp
    )
    add_windows_resources(bitcoin-wallet dash-wallet-res.rc)
    target_link_libraries(bitcoin-wallet
      core_interface
      bitcoin_wallet
      bitcoin_common
      bitcoin_util
      univalue
      Boost::headers
    )
    set_target_properties(bitcoin-wallet PROPERTIES OUTPUT_NAME dash-wallet)
    list(APPEND installable_targets bitcoin-wallet)
  endif()
endif()


# P2P and RPC server functionality used by `bitcoind` and `bitcoin-qt` executables.
add_library(bitcoin_node STATIC EXCLUDE_FROM_ALL
  active/context.cpp
  active/dkgsession.cpp
  active/dkgsessionhandler.cpp
  active/masternode.cpp
  addrdb.cpp
  addrman.cpp
  banman.cpp
  batchedlogger.cpp
  bip324.cpp
  blockencodings.cpp
  blockfilter.cpp
  chainlock/chainlock.cpp
  chainlock/clsig.cpp
  chainlock/handler.cpp
  chainlock/signing.cpp
  coinjoin/coinjoin.cpp
  coinjoin/server.cpp
  coinjoin/walletman.cpp
  chain.cpp
  consensus/tx_verify.cpp
  dbwrapper.cpp
  deploymentstatus.cpp
  dsnotificationinterface.cpp
  evo/assetlocktx.cpp
  evo/cbtx.cpp
  evo/creditpool.cpp
  evo/chainhelper.cpp
  evo/deterministicmns.cpp
  evo/dmnstate.cpp
  evo/evodb.cpp
  evo/mnauth.cpp
  evo/mnhftx.cpp
  evo/providertx.cpp
  evo/providertx_service.cpp
  evo/simplifiedmns.cpp
  evo/smldiff.cpp
  evo/specialtx.cpp
  evo/specialtx_filter.cpp
  evo/specialtxman.cpp
  flatfile.cpp
  governance/governance.cpp
  governance/net_governance.cpp
  governance/object.cpp
  governance/signing.cpp
  governance/superblock.cpp
  governance/vote.cpp
  governance/votedb.cpp
  gsl/assert.cpp
  httprpc.cpp
  httpserver.cpp
  i2p.cpp
  index/addressindex.cpp
  index/addressindex_util.cpp
  index/base.cpp
  index/blockfilterindex.cpp
  index/coinstatsindex.cpp
  index/spentindex.cpp
  index/timestampindex.cpp
  index/txindex.cpp
  init.cpp
  kernel/chain.cpp
  kernel/checks.cpp
  kernel/coinstats.cpp
  kernel/context.cpp
  kernel/mempool_persist.cpp
  instantsend/db.cpp
  instantsend/instantsend.cpp
  instantsend/lock.cpp
  instantsend/net_instantsend.cpp
  instantsend/signing.cpp
  llmq/blockprocessor.cpp
  llmq/commitment.cpp
  llmq/context.cpp
  llmq/debug.cpp
  llmq/dkgsession.cpp
  llmq/dkgsessionhandler.cpp
  llmq/dkgsessionmgr.cpp
  llmq/ehf_signals.cpp
  llmq/net_dkg.cpp
  llmq/net_quorum.cpp
  llmq/net_signing.cpp
  llmq/observer.cpp
  llmq/options.cpp
  llmq/quorums.cpp
  llmq/quorumsman.cpp
  llmq/signhash.cpp
  llmq/signing.cpp
  llmq/signing_shares.cpp
  llmq/snapshot.cpp
  llmq/utils.cpp
  mapport.cpp
  masternode/meta.cpp
  masternode/payments.cpp
  masternode/sync.cpp
  masternode/utils.cpp
  net.cpp
  netfulfilledman.cpp
  net_processing.cpp
  netgroup.cpp
  node/blockstorage.cpp
  node/caches.cpp
  node/chainstate.cpp
  node/chainstatemanager_args.cpp
  node/coin.cpp
  node/connection_types.cpp
  node/context.cpp
  node/eviction.cpp
  node/interface_ui.cpp
  node/interfaces.cpp
  node/mempool_args.cpp
  node/mempool_persist_args.cpp
  node/miner.cpp
  node/minisketchwrapper.cpp
  node/psbt.cpp
  node/sync_manager.cpp
  node/transaction.cpp
  node/txreconciliation.cpp
  node/utxo_snapshot.cpp
  node/validation_cache_args.cpp
  noui.cpp
  policy/fees.cpp
  policy/fees_args.cpp
  policy/packages.cpp
  policy/settings.cpp
  pow.cpp
  rest.cpp
  rpc/blockchain.cpp
  rpc/coinjoin.cpp
  rpc/evo.cpp
  rpc/fees.cpp
  rpc/governance.cpp
  rpc/json_help.cpp
  rpc/masternode.cpp
  rpc/mempool.cpp
  rpc/mining.cpp
  rpc/net.cpp
  rpc/node.cpp
  rpc/output_script.cpp
  rpc/quorums.cpp
  rpc/rawtransaction.cpp
  rpc/server.cpp
  rpc/server_util.cpp
  rpc/signmessage.cpp
  rpc/txoutproof.cpp
  script/sigcache.cpp
  shutdown.cpp
  spork.cpp
  stats/client.cpp
  stats/rawsender.cpp
  timedata.cpp
  torcontrol.cpp
  txdb.cpp
  txmempool.cpp
  txorphanage.cpp
  txrequest.cpp
  validation.cpp
  validationinterface.cpp
  versionbits.cpp
  $<$<TARGET_EXISTS:bitcoin_wallet>:wallet/init.cpp>
  $<$<NOT:$<TARGET_EXISTS:bitcoin_wallet>>:dummywallet.cpp>
)
target_link_libraries(bitcoin_node
  PRIVATE
    core_interface
    bitcoin_common
    bitcoin_util
    dashbls
    leveldb
    minisketch
    univalue
    Boost::headers
    $<TARGET_NAME_IF_EXISTS:libevent::libevent>
    $<TARGET_NAME_IF_EXISTS:libevent::pthreads>
    $<TARGET_NAME_IF_EXISTS:NATPMP::NATPMP>
    $<TARGET_NAME_IF_EXISTS:MiniUPnPc::MiniUPnPc>
    $<TARGET_NAME_IF_EXISTS:bitcoin_zmq>
    $<TARGET_NAME_IF_EXISTS:USDT::headers>
)


# Bitcoin Core bitcoind.
if(BUILD_DAEMON)
  add_executable(bitcoind
    bitcoind.cpp
    init/bitcoind.cpp
  )
  add_windows_resources(bitcoind dashd-res.rc)
  target_link_libraries(bitcoind
    core_interface
    bitcoin_node
    dashbls
    $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
  )
  set_target_properties(bitcoind PROPERTIES OUTPUT_NAME dashd)
  list(APPEND installable_targets bitcoind)
endif()
if(WITH_MULTIPROCESS)
  add_executable(bitcoin-node
    bitcoind.cpp
    init/bitcoin-node.cpp
  )
  target_link_libraries(bitcoin-node
    core_interface
    bitcoin_node
    bitcoin_ipc
    dashbls
    $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
  )
  set_target_properties(bitcoin-node PROPERTIES OUTPUT_NAME dash-node)
  list(APPEND installable_targets bitcoin-node)
endif()


add_library(bitcoin_cli STATIC EXCLUDE_FROM_ALL
  compat/stdin.cpp
  rpc/client.cpp
)
target_link_libraries(bitcoin_cli
  PUBLIC
    core_interface
    univalue
)


# Bitcoin Core RPC client
if(BUILD_CLI)
  add_executable(bitcoin-cli bitcoin-cli.cpp)
  add_windows_resources(bitcoin-cli dash-cli-res.rc)
  target_link_libraries(bitcoin-cli
    core_interface
    bitcoin_cli
    bitcoin_common
    bitcoin_util
    $<TARGET_NAME_IF_EXISTS:libevent::libevent>
  )
  set_target_properties(bitcoin-cli PROPERTIES OUTPUT_NAME dash-cli)
  list(APPEND installable_targets bitcoin-cli)
endif()


if(BUILD_TX)
  add_executable(bitcoin-tx bitcoin-tx.cpp)
  add_windows_resources(bitcoin-tx dash-tx-res.rc)
  target_link_libraries(bitcoin-tx
    core_interface
    bitcoin_common
    bitcoin_util
    univalue
  )
  set_target_properties(bitcoin-tx PROPERTIES OUTPUT_NAME dash-tx)
  list(APPEND installable_targets bitcoin-tx)
endif()


if(BUILD_UTIL)
  add_executable(bitcoin-util bitcoin-util.cpp)
  add_windows_resources(bitcoin-util bitcoin-util-res.rc)
  target_link_libraries(bitcoin-util
    core_interface
    bitcoin_common
    bitcoin_util
  )
  set_target_properties(bitcoin-util PROPERTIES OUTPUT_NAME dash-util)
  list(APPEND installable_targets bitcoin-util)
endif()


if(BUILD_GUI)
  add_subdirectory(qt)
endif()


if(BUILD_UTIL_CHAINSTATE)
  add_executable(bitcoin-chainstate
    bitcoin-chainstate.cpp
  )
  # TODO: The `SKIP_BUILD_RPATH` property setting can be deleted
  #       in the future after reordering Guix script commands to
  #       perform binary checks after the installation step.
  # Relevant discussions:
  # - https://github.com/hebasto/bitcoin/pull/236#issuecomment-2183120953
  # - https://github.com/bitcoin/bitcoin/pull/30312#issuecomment-2191235833
  set_target_properties(bitcoin-chainstate PROPERTIES
    SKIP_BUILD_RPATH OFF
  )
  target_link_libraries(bitcoin-chainstate
    PRIVATE
      core_interface
      bitcoin_common
      bitcoin_node
      bitcoin_util
      Boost::headers
      dashbls
      leveldb
      univalue
      $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
  )
  set_target_properties(bitcoin-chainstate PROPERTIES OUTPUT_NAME dash-chainstate)
  list(APPEND installable_targets bitcoin-chainstate)
endif()


add_subdirectory(test/util)
if(BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(BUILD_TESTS)
  add_subdirectory(test)
endif()

if(BUILD_FUZZ_BINARY)
  add_subdirectory(test/fuzz)
endif()


install(TARGETS ${installable_targets}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
unset(installable_targets)

if(INSTALL_MAN)
  # TODO: these stubs are no longer needed. man pages should be generated at install time.
  install(DIRECTORY ../doc/man/
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    FILES_MATCHING PATTERN *.1
  )
endif()
