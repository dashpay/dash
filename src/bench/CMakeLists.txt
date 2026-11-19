# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_executable(bench_bitcoin
  bench_bitcoin.cpp
  bench.cpp
  nanobench.cpp
# Benchmarks:
  addrman.cpp
  base58.cpp
  bech32.cpp
  bip324_ecdh.cpp
  bls.cpp
  bls_dkg.cpp
  bls_pubkey_agg.cpp
  block_assemble.cpp
  blockfilter_index.cpp
  ccoins_caching.cpp
  chacha20.cpp
  checkblock.cpp
  checkblockindex.cpp
  checkqueue.cpp
  disconnected_transactions.cpp
  crypto_hash.cpp
  descriptors.cpp
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
  net_processing.cpp
  parse_hex.cpp
  peer_eviction.cpp
  poly1305.cpp
  pool.cpp
  pow_hash.cpp
  prevector.cpp
  readblock.cpp
  rollingbloom.cpp
  rpc_blockchain.cpp
  rpc_mempool.cpp
  streams_findbyte.cpp
  strencodings.cpp
  string_cast.cpp
  util_time.cpp
  verify_script.cpp
  xor.cpp
)
set_target_properties(bench_bitcoin PROPERTIES OUTPUT_NAME bench_dash)

include(TargetDataSources)
target_raw_data_sources(bench_bitcoin NAMESPACE benchmark::data
  data/block813851.raw
)

target_link_libraries(bench_bitcoin
  core_interface
  test_util
  bitcoin_node
  Boost::headers
  blst
  leveldb
)

if(ENABLE_WALLET)
  target_sources(bench_bitcoin
    PRIVATE
      coin_selection.cpp
      wallet_balance.cpp
      wallet_create.cpp
      wallet_create_tx.cpp
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
