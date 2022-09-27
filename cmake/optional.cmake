# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Optional features and packages.

if(CCACHE)
  find_program(PROG_CCACHE ccache)
  if(PROG_CCACHE)
    set(CCACHE ON)
    if(MSVC)
      # See https://github.com/ccache/ccache/wiki/MS-Visual-Studio
      set(MSVC_CCACHE_WRAPPER_CONTENT "\"${PROG_CCACHE}\" \"${CMAKE_CXX_COMPILER}\"")
      set(MSVC_CCACHE_WRAPPER_FILENAME wrapped-cl.bat)
      file(WRITE ${CMAKE_BINARY_DIR}/${MSVC_CCACHE_WRAPPER_FILENAME} "${MSVC_CCACHE_WRAPPER_CONTENT} %*")
      set(CMAKE_VS_GLOBALS
        "CLToolExe=${MSVC_CCACHE_WRAPPER_FILENAME}"
        "CLToolPath=${CMAKE_BINARY_DIR}"
        "TrackFileAccess=false"
        "UseMultiToolTask=true"
        "DebugInformationFormat=OldStyle"
      )
    else()
      set(CMAKE_C_COMPILER_LAUNCHER ${PROG_CCACHE})
      set(CMAKE_CXX_COMPILER_LAUNCHER ${PROG_CCACHE})
    endif()
  elseif(CCACHE STREQUAL ON)
    message(FATAL_ERROR "ccache requested, but not found.")
  else()
    set(CCACHE OFF)
  endif()
  mark_as_advanced(PROG_CCACHE)
endif()

if(ENABLE_WALLET)
  if(WITH_SQLITE)
    pkg_check_modules(sqlite sqlite3>=3.7.17 IMPORTED_TARGET)
    if(sqlite_FOUND)
      set(WITH_SQLITE ON)
      set(USE_SQLITE ON)
    else()
      if(WITH_SQLITE STREQUAL ON)
        message(FATAL_ERROR "SQLite requested, but not found.")
      endif()
      set(WITH_SQLITE OFF)
    endif()
  endif()

  if(WITH_BDB)
    find_package(BerkeleyDB 4.8)
    if(BerkeleyDB_FOUND)
      set(WITH_BDB ON)
      set(USE_BDB ON)
      if(NOT BerkeleyDB_VERSION VERSION_EQUAL 4.8)
        message(WARNING "Found Berkeley DB (BDB) other than 4.8.")
        if(WARN_INCOMPATIBLE_BDB)
          message(WARNING "BDB (legacy) wallets opened by this build would not be portable!\n"
                          "If this is intended, pass \"-DWARN_INCOMPATIBLE_BDB=OFF\".\n"
                          "Passing \"-DWITH_BDB=OFF\" will suppress this warning.\n")
        else()
          message(WARNING "BDB (legacy) wallets opened by this build will not be portable!")
        endif()
      endif()
    else()
      message(WARNING "Berkeley DB (BDB) required for legacy wallet support, but not found.\n"
                      "Passing \"-DWITH_BDB=OFF\" will suppress this warning.\n")
      set(WITH_BDB OFF)
    endif()
  endif()
else()
  set(WITH_SQLITE OFF)
  set(WITH_BDB OFF)
endif()

if(WITH_NATPMP)
  find_package(NATPMP)
  if(NATPMP_FOUND)
    set(WITH_NATPMP ON)
  else()
    if(WITH_NATPMP STREQUAL ON)
      message(FATAL_ERROR "libnatpmp requested, but not found.")
    else()
      message(WARNING "libnatpmp not found, disabling.\n"
                      "To skip libnatpmp check, use \"-DWITH_NATPMP=OFF\".\n")
    endif()
    set(WITH_NATPMP OFF)
  endif()
endif()

if(WITH_MINIUPNPC)
  find_package(MiniUPnPc)
  if(MiniUPnPc_FOUND)
    set(WITH_MINIUPNPC ON)
  else()
    if(WITH_MINIUPNPC STREQUAL ON)
      message(FATAL_ERROR "libminiupnpc requested, but not found.")
    else()
      message(WARNING "libminiupnpc not found, disabling.\n"
                      "To skip libminiupnpc check, use \"-DWITH_MINIUPNPC=OFF\".\n")
    endif()
    set(WITH_MINIUPNPC OFF)
  endif()
endif()
