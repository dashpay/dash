# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# The dashbls subtree's own CMake project downloads blst at configure time, so
# build its sources here instead and take blst from depends or the system.

if(ENABLE_MIMALLOC)
  add_library(mimalloc_secure STATIC EXCLUDE_FROM_ALL
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/alloc-aligned.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/alloc-posix.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/alloc.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/arena-meta.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/arena.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/bitmap.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/heap.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/init.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/libc.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/options.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/os.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/page-map.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/page.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/random.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/stats.c
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src/prim/prim.c
  )
  target_compile_definitions(mimalloc_secure
    PRIVATE
      MI_SECURE=4
      NDEBUG
  )
  target_include_directories(mimalloc_secure
    PRIVATE
      ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/include
      ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/src
  )
  target_link_libraries(mimalloc_secure
    PRIVATE
      core_interface
    PUBLIC
      $<$<PLATFORM_ID:Windows>:psapi;bcrypt;advapi32>
  )
  set_target_properties(mimalloc_secure PROPERTIES
    EXPORT_COMPILE_COMMANDS OFF
  )
endif()

add_library(dashbls STATIC EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/src/dashbls/src/bls.cpp
  ${PROJECT_SOURCE_DIR}/src/dashbls/src/chaincode.cpp
  ${PROJECT_SOURCE_DIR}/src/dashbls/src/elements.cpp
  ${PROJECT_SOURCE_DIR}/src/dashbls/src/extendedprivatekey.cpp
  ${PROJECT_SOURCE_DIR}/src/dashbls/src/extendedpublickey.cpp
  ${PROJECT_SOURCE_DIR}/src/dashbls/src/legacy.cpp
  ${PROJECT_SOURCE_DIR}/src/dashbls/src/privatekey.cpp
  ${PROJECT_SOURCE_DIR}/src/dashbls/src/schemes.cpp
  ${PROJECT_SOURCE_DIR}/src/dashbls/src/threshold.cpp
)
target_compile_definitions(dashbls
  PRIVATE
    $<$<BOOL:${ENABLE_MIMALLOC}>:BLSALLOC_MIMALLOC=1>
)
# The vendored headers are included as system headers, so that Dash's warning
# set (and WERROR) does not apply to them.
target_include_directories(dashbls SYSTEM
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src/dashbls/include/dashbls
    ${PROJECT_SOURCE_DIR}/src/dashbls/depends/mimalloc/include
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/dashbls/include>
)
target_link_libraries(dashbls
  PRIVATE
    core_interface
  PUBLIC
    $<TARGET_NAME_IF_EXISTS:mimalloc_secure>
    BLST::BLST
)
set_target_properties(dashbls PROPERTIES
  EXPORT_COMPILE_COMMANDS OFF
)

if(BUILD_TESTS)
  add_executable(dashbls_runtest
    ${PROJECT_SOURCE_DIR}/src/dashbls/src/test.cpp
  )
  target_compile_definitions(dashbls_runtest
    PRIVATE
      $<$<BOOL:${ENABLE_MIMALLOC}>:BLSALLOC_MIMALLOC=1>
      DASHBLS_REFERENCE_VECTORS="${PROJECT_SOURCE_DIR}/src/dashbls/test-vectors/reference.txt"
  )
  target_include_directories(dashbls_runtest SYSTEM
    PRIVATE
      ${PROJECT_SOURCE_DIR}/src/dashbls/include/dashbls
      ${PROJECT_SOURCE_DIR}/src/dashbls/depends/catch2/include
  )
  target_link_libraries(dashbls_runtest
    PRIVATE
      core_interface
      dashbls
  )
  set_target_properties(dashbls_runtest PROPERTIES
    EXPORT_COMPILE_COMMANDS OFF
  )
  add_test(NAME dashbls_runtest COMMAND dashbls_runtest)
endif()
