# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

if(TARGET libzmq)
  add_library(bitcoin_zmq STATIC EXCLUDE_FROM_ALL "")
  target_sources(bitcoin_zmq
    PRIVATE
      zmqabstractnotifier.cpp
      zmqnotificationinterface.cpp
      zmqpublishnotifier.cpp
      zmqrpc.cpp
      zmqutil.cpp
  )
  target_compile_definitions(bitcoin_zmq
    INTERFACE
      ENABLE_ZMQ=1
  )
  target_include_directories(bitcoin_zmq
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
  )
  target_link_libraries(bitcoin_zmq
    PRIVATE
      bitcoin_consensus
      libzmq
      leveldb
      univalue
      $<$<PLATFORM_ID:Windows>:iphlpapi>
      $<$<PLATFORM_ID:Windows>:ws2_32>
  )
endif()
