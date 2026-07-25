# Copyright (c) 2024-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

add_executable(test_bitcoin-qt
  apptests.cpp
  optiontests.cpp
  rpcnestedtests.cpp
  test_main.cpp
  trafficgraphdatatests.cpp
  uritests.cpp
  util.cpp
  ../../init/bitcoin-qt.cpp
)

set_target_properties(test_bitcoin-qt PROPERTIES OUTPUT_NAME test_dash-qt)

target_link_libraries(test_bitcoin-qt
  core_interface
  bitcoinqt
  test_util
  bitcoin_node
  # The Dash-specific headers reachable from these tests include dbwrapper.h,
  # so the LevelDB headers have to be on the include path.
  leveldb
  dashbls
  Boost::headers
  Qt5::Test
)

import_plugins(test_bitcoin-qt)

if(ENABLE_WALLET)
  target_sources(test_bitcoin-qt
    PRIVATE
      addressbooktests.cpp
      wallettests.cpp
      ../../wallet/test/wallet_test_fixture.cpp
  )
endif()

if(NOT QT_IS_STATIC)
  add_custom_command(
    TARGET test_bitcoin-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PROPERTY:Qt5::QMinimalIntegrationPlugin,LOCATION_$<UPPER_CASE:$<CONFIG>>> $<TARGET_FILE_DIR:test_bitcoin-qt>/plugins/platforms
    VERBATIM
  )
endif()

add_test(NAME test_bitcoin-qt
  COMMAND test_bitcoin-qt
)
if(WIN32 AND VCPKG_TARGET_TRIPLET)
  # On Windows, vcpkg configures Qt with `-opengl dynamic`, which makes
  # the "minimal" platform plugin unusable due to internal Qt bugs.
  set_tests_properties(test_bitcoin-qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows"
  )
endif()

install(TARGETS test_bitcoin-qt
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
