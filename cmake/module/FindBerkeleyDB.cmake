# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  execute_process(
    COMMAND brew --prefix berkeley-db@4
    OUTPUT_VARIABLE bdb4_brew_prefix
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

find_path(BerkeleyDB_INCLUDE_DIR
  NAMES db.h
  HINTS ${bdb4_brew_prefix}/include
  PATH_SUFFIXES 4.8 48 4 db4 5 5.3 db5
)

if(BerkeleyDB_INCLUDE_DIR)
  file(
    STRINGS "${BerkeleyDB_INCLUDE_DIR}/db.h" version_strings
    REGEX ".*DB_VERSION_(MAJOR|MINOR)[ \t]+[0-9]+.*"
  )
  string(REGEX REPLACE ".*DB_VERSION_MAJOR[ \t]+([0-9]+).*" "\\1" BerkeleyDB_VERSION_MAJOR "${version_strings}")
  string(REGEX REPLACE ".*DB_VERSION_MINOR[ \t]+([0-9]+).*" "\\1" BerkeleyDB_VERSION_MINOR "${version_strings}")
  set(BerkeleyDB_VERSION ${BerkeleyDB_VERSION_MAJOR}.${BerkeleyDB_VERSION_MINOR})
endif()

find_library(BerkeleyDB_LIBRARY
  NAMES db_cxx-4.8 libdb48 db4_cxx db_cxx db_cxx-5
  HINTS ${bdb4_brew_prefix}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(BerkeleyDB
  REQUIRED_VARS BerkeleyDB_LIBRARY BerkeleyDB_INCLUDE_DIR
  VERSION_VAR BerkeleyDB_VERSION
)

if(BerkeleyDB_FOUND AND NOT TARGET BerkeleyDB::BerkeleyDB)
  add_library(BerkeleyDB::BerkeleyDB UNKNOWN IMPORTED)
  set_target_properties(BerkeleyDB::BerkeleyDB PROPERTIES
    IMPORTED_LOCATION "${BerkeleyDB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${BerkeleyDB_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(
  BerkeleyDB_INCLUDE_DIR
  BerkeleyDB_LIBRARY
)
