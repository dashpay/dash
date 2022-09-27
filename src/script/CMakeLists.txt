# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

if(BUILD_SHARED)
  add_library(bitcoinconsensus SHARED "")
  target_sources(bitcoinconsensus
    PRIVATE
      bitcoinconsensus.cpp
      ../support/cleanse.cpp
      $<TARGET_OBJECTS:bitcoin_consensus>
  )
  target_compile_definitions(bitcoinconsensus
    PRIVATE
      BUILD_BITCOIN_INTERNAL
  )
  target_include_directories(bitcoinconsensus
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
  )
  target_link_libraries(bitcoinconsensus
    PRIVATE
      bitcoin_crypto
      secp256k1
  )
  set_target_properties(bitcoinconsensus PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/bitcoinconsensus.h"
  )
  set_target_properties(bitcoinconsensus PROPERTIES
    SOVERSION 0
    VERSION 0.0.0
  )
endif()

if(BUILD_STATIC)
  add_library(bitcoinconsensus_static STATIC "")
  target_sources(bitcoinconsensus_static
    PRIVATE
      bitcoinconsensus.cpp
      ../support/cleanse.cpp
      $<TARGET_OBJECTS:bitcoin_consensus>
  )
  target_compile_definitions(bitcoinconsensus_static
    PRIVATE
      BUILD_BITCOIN_INTERNAL
  )
  target_include_directories(bitcoinconsensus_static
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
  )
  set_target_properties(bitcoinconsensus_static PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/bitcoinconsensus.h"
  )
  if(NOT MSVC)
    set_target_properties(bitcoinconsensus_static PROPERTIES
      OUTPUT_NAME bitcoinconsensus
    )
  endif()
endif()

set(prefix ${CMAKE_INSTALL_PREFIX})
set(exec_prefix "\${prefix}")
set(libdir "\${exec_prefix}/lib")
set(includedir "\${prefix}/include")
configure_file(${CMAKE_SOURCE_DIR}/libdashconsensus.pc.in libbitcoinconsensus.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libbitcoinconsensus.pc DESTINATION lib/pkgconfig)
