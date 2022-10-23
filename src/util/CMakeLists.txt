# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Home for common functionality shared by different executables and libraries.
# Similar to `bitcoin_common` library, but lower-level.
add_library(bitcoin_util STATIC EXCLUDE_FROM_ALL "")
target_sources(bitcoin_util
  PRIVATE
    asmap.cpp
    bip32.cpp
    bytevectorhash.cpp
    error.cpp
    fees.cpp
    getuniquepath.cpp
    message.cpp
    moneystr.cpp
    settings.cpp
    serfloat.cpp
    spanparsing.cpp
    strencodings.cpp
    string.cpp
    system.cpp
    threadnames.cpp
    time.cpp
    ../chainparamsbase.cpp
    ../clientversion.cpp
    ../fs.cpp
    ../interfaces/handler.cpp
    ../logging.cpp
    ../random.cpp
    ../randomenv.cpp
    ../rpc/request.cpp
    ../support/cleanse.cpp
    ../support/lockedpool.cpp
    ../sync.cpp
    ../threadinterrupt.cpp
)
target_compile_definitions(bitcoin_util
  PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING>
)
target_include_directories(bitcoin_util
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)
target_link_libraries(bitcoin_util
  PRIVATE
    bitcoin_crypto
    univalue
    Boost::headers
    Threads::Threads
)

if(TARGET PkgConfig::libevent)
  target_sources(bitcoin_util PRIVATE url.cpp)
  target_link_libraries(bitcoin_util PRIVATE PkgConfig::libevent)
endif()

if(TARGET std_filesystem)
  target_link_libraries(bitcoin_util PRIVATE std_filesystem)
endif()
