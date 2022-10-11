# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

include(GenerateHeaders)
generate_header_from_json(data/base58_encode_decode.json)
generate_header_from_json(data/bip341_wallet_vectors.json)
generate_header_from_json(data/blockfilters.json)
generate_header_from_json(data/key_io_invalid.json)
generate_header_from_json(data/key_io_valid.json)
generate_header_from_json(data/script_tests.json)
generate_header_from_json(data/sighash.json)
generate_header_from_json(data/tx_invalid.json)
generate_header_from_json(data/tx_valid.json)
generate_header_from_raw(data/asmap.raw)

add_executable(test_bitcoin "")
target_sources(test_bitcoin
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:compilerbug_tests.cpp>
    ${CMAKE_CURRENT_BINARY_DIR}/data/asmap.raw.h
    ${CMAKE_CURRENT_BINARY_DIR}/data/base58_encode_decode.json.h
    ${CMAKE_CURRENT_BINARY_DIR}/data/bip341_wallet_vectors.json.h
    ${CMAKE_CURRENT_BINARY_DIR}/data/blockfilters.json.h
    ${CMAKE_CURRENT_BINARY_DIR}/data/key_io_invalid.json.h
    ${CMAKE_CURRENT_BINARY_DIR}/data/key_io_valid.json.h
    ${CMAKE_CURRENT_BINARY_DIR}/data/script_tests.json.h
    ${CMAKE_CURRENT_BINARY_DIR}/data/sighash.json.h
    ${CMAKE_CURRENT_BINARY_DIR}/data/tx_invalid.json.h
    ${CMAKE_CURRENT_BINARY_DIR}/data/tx_valid.json.h
    addrman_tests.cpp
    allocator_tests.cpp
    amount_tests.cpp
    arith_uint256_tests.cpp
    base32_tests.cpp
    base58_tests.cpp
    base64_tests.cpp
    bech32_tests.cpp
    bip32_tests.cpp
    blockchain_tests.cpp
    blockencodings_tests.cpp
    blockfilter_index_tests.cpp
    blockfilter_tests.cpp
    bloom_tests.cpp
    bswap_tests.cpp
    checkqueue_tests.cpp
    coins_tests.cpp
    compress_tests.cpp
    crypto_tests.cpp
    cuckoocache_tests.cpp
    dbwrapper_tests.cpp
    denialofservice_tests.cpp
    descriptor_tests.cpp
    flatfile_tests.cpp
    fs_tests.cpp
    getarg_tests.cpp
    hash_tests.cpp
    key_io_tests.cpp
    key_tests.cpp
    logging_tests.cpp
    main.cpp
    mempool_tests.cpp
    merkleblock_tests.cpp
    merkle_tests.cpp
    miner_tests.cpp
    multisig_tests.cpp
    netbase_tests.cpp
    net_tests.cpp
    pmt_tests.cpp
    policyestimator_tests.cpp
    pow_tests.cpp
    prevector_tests.cpp
    raii_event_tests.cpp
    random_tests.cpp
    reverselock_tests.cpp
    rpc_tests.cpp
    sanity_tests.cpp
    scheduler_tests.cpp
    scriptnum10.h
    scriptnum_tests.cpp
    script_p2sh_tests.cpp
    script_standard_tests.cpp
    script_tests.cpp
    serialize_tests.cpp
    settings_tests.cpp
    sighash_tests.cpp
    sigopcount_tests.cpp
    skiplist_tests.cpp
    streams_tests.cpp
    sync_tests.cpp
    timedata_tests.cpp
    torcontrol_tests.cpp
    transaction_tests.cpp
    txindex_tests.cpp
    txvalidationcache_tests.cpp
    txvalidation_tests.cpp
    uint256_tests.cpp
    util_tests.cpp
    util_threadnames_tests.cpp
    validation_block_tests.cpp
    validation_chainstatemanager_tests.cpp
    validation_chainstate_tests.cpp
    validation_flush_tests.cpp
    validation_tests.cpp
    versionbits_tests.cpp
)
target_include_directories(test_bitcoin
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

if(TARGET bitcoinconsensus AND NOT WIN32)
  target_link_libraries(test_bitcoin PRIVATE bitcoinconsensus)
elseif(TARGET bitcoinconsensus_static)
  target_link_libraries(test_bitcoin PRIVATE bitcoinconsensus_static)
endif()
target_link_libraries(test_bitcoin
  PRIVATE
    test_util
    rpc_client
    bitcoin_node
    bitcoin_common
    bitcoin_consensus
    bitcoin_util
    minisketch
    leveldb
    univalue
    Boost::headers
    PkgConfig::libevent
)

if(ENABLE_WALLET)
  target_sources(test_bitcoin
    PRIVATE
      ../wallet/test/coinselector_tests.cpp
      ../wallet/test/init_test_fixture.cpp
      ../wallet/test/init_tests.cpp
      ../wallet/test/ismine_tests.cpp
      ../wallet/test/psbt_wallet_tests.cpp
      ../wallet/test/wallet_crypto_tests.cpp
      ../wallet/test/wallet_test_fixture.cpp
      ../wallet/test/wallet_tests.cpp
  )
  target_link_libraries(test_bitcoin PRIVATE bitcoin_wallet)
  if(USE_BDB)
    target_sources(test_bitcoin PRIVATE ../wallet/test/db_tests.cpp)
    target_link_libraries(test_bitcoin PRIVATE BerkeleyDB::BerkeleyDB)
  endif()
endif()

function(add_boost_test source_file_name)
  set(source_file_path "${CMAKE_CURRENT_SOURCE_DIR}/${source_file_name}")
  if(NOT EXISTS ${source_file_path})
    return()
  endif()

  file(READ "${source_file_path}" source_file_content)
  string(REGEX
    MATCH "(BOOST_FIXTURE_TEST_SUITE|BOOST_AUTO_TEST_SUITE)\\(([A-Za-z0-9_]+)"
    test_suite_macro "${source_file_content}"
  )
  string(REGEX
    REPLACE "(BOOST_FIXTURE_TEST_SUITE|BOOST_AUTO_TEST_SUITE)\\(" ""
    test_suite_name "${test_suite_macro}"
  )
  if(test_suite_name)
    add_test(NAME "${test_suite_name}:${source_file_name}"
      COMMAND test_bitcoin --run_test=${test_suite_name} --catch_system_error=no
    )
  endif()
endfunction()

function(add_all_test_targets)
  get_target_property(test_sources test_bitcoin SOURCES)
  foreach(test_source ${test_sources})
    add_boost_test(${test_source})
  endforeach()
endfunction()

include(CTest)
enable_testing()
add_all_test_targets()
