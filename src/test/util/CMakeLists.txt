# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

add_library(test_util STATIC EXCLUDE_FROM_ALL "")
target_sources(test_util
  PRIVATE
    blockfilter.cpp
    logging.cpp
    net.cpp
    setup_common.cpp
    str.cpp
    transaction_utils.cpp
)
target_include_directories(test_util
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)
target_link_libraries(test_util
  PRIVATE
    bitcoin_node
    leveldb
    univalue
    Boost::headers
)
