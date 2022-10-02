# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Wallet functionality used by bitcoind and bitcoin-wallet executables.
add_library(bitcoin_wallet STATIC EXCLUDE_FROM_ALL "")
target_sources(bitcoin_wallet
  PRIVATE
    coincontrol.cpp
    coinselection.cpp
    context.cpp
    crypter.cpp
    db.cpp
    fees.cpp
    load.cpp
    scriptpubkeyman.cpp
    wallet.cpp
    walletdb.cpp
    walletutil.cpp
)
target_include_directories(bitcoin_wallet
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)
target_link_libraries(bitcoin_wallet
  PRIVATE
    bitcoin_common
    univalue
    Boost::headers
)

if(NOT USE_SQLITE AND NOT USE_BDB)
  message(FATAL_ERROR "Wallet functionality requested but no BDB or SQLite support available.")
endif()
if(USE_SQLITE)
  target_sources(bitcoin_wallet PRIVATE)
  target_link_libraries(bitcoin_wallet PRIVATE PkgConfig::sqlite)
endif()
if(USE_BDB)
  target_sources(bitcoin_wallet PRIVATE bdb.cpp salvage.cpp)
  target_link_libraries(bitcoin_wallet PRIVATE BerkeleyDB::BerkeleyDB)
endif()
