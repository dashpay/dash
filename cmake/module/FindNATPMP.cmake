# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  execute_process(
    COMMAND brew --prefix libnatpmp
    OUTPUT_VARIABLE natpmp_brew_prefix
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

find_path(NATPMP_INCLUDE_DIR
  NAMES natpmp.h
  HINTS ${natpmp_brew_prefix}/include
)

find_library(NATPMP_LIBRARY
  NAMES natpmp
  HINTS ${natpmp_brew_prefix}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NATPMP
  REQUIRED_VARS NATPMP_LIBRARY NATPMP_INCLUDE_DIR
)

if(NATPMP_FOUND AND NOT TARGET NATPMP::NATPMP)
  add_library(NATPMP::NATPMP UNKNOWN IMPORTED)
  set_target_properties(NATPMP::NATPMP PROPERTIES
    IMPORTED_LOCATION "${NATPMP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${NATPMP_INCLUDE_DIR}"
  )
  set_property(TARGET NATPMP::NATPMP PROPERTY
    INTERFACE_COMPILE_DEFINITIONS USE_NATPMP=$<BOOL:${ENABLE_NATPMP_DEFAULT}> $<$<PLATFORM_ID:Windows>:NATPMP_STATICLIB>
  )
endif()

mark_as_advanced(
  NATPMP_INCLUDE_DIR
  NATPMP_LIBRARY
)
