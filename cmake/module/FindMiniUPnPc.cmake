# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  execute_process(
    COMMAND brew --prefix miniupnpc
    OUTPUT_VARIABLE miniupnp_brew_prefix
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
else()
  find_package(PkgConfig)
  pkg_check_modules(PC_MiniUPnPc QUIET miniupnpc)
endif()

find_path(MiniUPnPc_INCLUDE_DIR
  NAMES miniupnpc/miniupnpc.h
  PATHS ${PC_MiniUPnPc_INCLUDE_DIRS}
  HINTS ${miniupnp_brew_prefix}/include
)

find_library(MiniUPnPc_LIBRARY
  NAMES miniupnpc
  PATHS ${PC_MiniUPnPc_LIBRARY_DIRS}
  HINTS ${miniupnp_brew_prefix}/lib
)

if(MiniUPnPc_INCLUDE_DIR)
  file(
    STRINGS "${MiniUPnPc_INCLUDE_DIR}/miniupnpc/miniupnpc.h" version_strings
    REGEX "^#define[\t ]+MINIUPNPC_API_VERSION[\t ]+[0-9]+"
  )
  string(REGEX REPLACE "^#define[\t ]+MINIUPNPC_API_VERSION[\t ]+([0-9]+)" "\\1" MiniUPnPc_API_VERSION "${version_strings}")

  # The minimum supported miniUPnPc API version is set to 10. This keeps compatibility
  # with Ubuntu 16.04 LTS and Debian 8 libminiupnpc-dev packages.
  if(MiniUPnPc_API_VERSION GREATER_EQUAL 10)
    set(MiniUPnPc_API_VERSION_OK TRUE)
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MiniUPnPc
  REQUIRED_VARS MiniUPnPc_LIBRARY MiniUPnPc_INCLUDE_DIR MiniUPnPc_API_VERSION_OK
)

if(MiniUPnPc_FOUND AND NOT TARGET MiniUPnPc::MiniUPnPc)
  add_library(MiniUPnPc::MiniUPnPc UNKNOWN IMPORTED)
  set_target_properties(MiniUPnPc::MiniUPnPc PROPERTIES
    IMPORTED_LOCATION "${MiniUPnPc_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MiniUPnPc_INCLUDE_DIR}"
  )
  set_property(TARGET MiniUPnPc::MiniUPnPc PROPERTY
    INTERFACE_COMPILE_DEFINITIONS USE_UPNP=$<BOOL:${ENABLE_UPNP_DEFAULT}> $<$<PLATFORM_ID:Windows>:MINIUPNP_STATICLIB>
  )
endif()

mark_as_advanced(
  MiniUPnPc_INCLUDE_DIR
  MiniUPnPc_LIBRARY
)
