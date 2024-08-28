# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include(GenerateHeaders)
generate_header_from_raw(data/block813851.raw)

add_executable(bench_bitcoin
  bench_bitcoin.cpp
  bench.cpp
  data.cpp
  nanobench.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/data/block813851.raw.h
# Benchmarks:
  addrman.cpp
  base58.cpp
  bech32.cpp
  bip324_ecdh.cpp
  bls.cpp
  bls_dkg.cpp
  bls_pubkey_agg.cpp
  block_assemble.cpp
  ccoins_caching.cpp
  chacha20.cpp
  checkblock.cpp
  checkqueue.cpp
  crypto_hash.cpp
  duplicate_inputs.cpp
  ecdsa.cpp
  ellswift.cpp
  examples.cpp
  gcs_filter.cpp
  hashpadding.cpp
  load_external.cpp
  lockedpool.cpp
  logging.cpp
  mempool_eviction.cpp
  mempool_stress.cpp
  merkle_root.cpp
  peer_eviction.cpp
  poly1305.cpp
  pool.cpp
  pow_hash.cpp
  prevector.cpp
  rollingbloom.cpp
  rpc_blockchain.cpp
  rpc_mempool.cpp
  strencodings.cpp
  string_cast.cpp
  util_time.cpp
  verify_script.cpp
)
set_target_properties(bench_bitcoin PROPERTIES OUTPUT_NAME bench_dash)

target_link_libraries(bench_bitcoin
  core_interface
  test_util
  bitcoin_node
  Boost::headers
  dashbls
  leveldb
)

if(ENABLE_WALLET)
  target_sources(bench_bitcoin
    PRIVATE
      coin_selection.cpp
      wallet_balance.cpp
      wallet_create.cpp
      wallet_loading.cpp
  )
  target_link_libraries(bench_bitcoin bitcoin_wallet)
endif()

add_test(NAME bench_sanity_check_high_priority
  COMMAND bench_bitcoin -sanity-check -priority-level=high
)

install(TARGETS bench_bitcoin
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
