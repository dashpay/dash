# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

find_path(BLST_INCLUDE_DIR
  NAMES blst.h
)

find_library(BLST_LIBRARY
  NAMES blst
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(BLST
  REQUIRED_VARS BLST_LIBRARY BLST_INCLUDE_DIR
)

if(BLST_FOUND AND NOT TARGET BLST::BLST)
  add_library(BLST::BLST UNKNOWN IMPORTED)
  set_target_properties(BLST::BLST PROPERTIES
    IMPORTED_LOCATION "${BLST_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${BLST_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(
  BLST_INCLUDE_DIR
  BLST_LIBRARY
)
