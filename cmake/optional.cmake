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
