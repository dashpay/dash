# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# This script is a multiplatform port of the share/genbuild.sh shell script.

macro(fatal_error)
  message(FATAL_ERROR "\n"
    "Usage:\n"
    "  cmake -D BUILD_INFO_HEADER_PATH=<path> [-D SOURCE_DIR=<path>] -P ${CMAKE_CURRENT_LIST_FILE}\n"
    "All specified paths must be absolute ones.\n"
  )
endmacro()

if(DEFINED BUILD_INFO_HEADER_PATH AND IS_ABSOLUTE "${BUILD_INFO_HEADER_PATH}")
  if(EXISTS "${BUILD_INFO_HEADER_PATH}")
    file(STRINGS ${BUILD_INFO_HEADER_PATH} INFO LIMIT_COUNT 1)
  endif()
else()
  fatal_error()
endif()

if(DEFINED SOURCE_DIR)
  if(IS_ABSOLUTE "${SOURCE_DIR}" AND IS_DIRECTORY "${SOURCE_DIR}")
    set(WORKING_DIR ${SOURCE_DIR})
  else()
    fatal_error()
  endif()
else()
  set(WORKING_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# NOTE: Unlike upstream, clientversion.cpp only knows about
#       BUILD_GIT_DESCRIPTION, so this emits the same `git describe` string
#       that share/genbuild.sh produces. Otherwise every non-release build
#       reports its version as "vX.Y.Z-unk".
set(GIT_DESCRIPTION)
if(NOT "$ENV{BITCOIN_GENBUILD_NO_GIT}" STREQUAL "1")
  find_package(Git QUIET)
  if(Git_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --is-inside-work-tree
      WORKING_DIRECTORY ${WORKING_DIR}
      OUTPUT_VARIABLE IS_INSIDE_WORK_TREE
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(IS_INSIDE_WORK_TREE)
      # Clean 'dirty' status of touched files that haven't been modified.
      execute_process(
        COMMAND ${GIT_EXECUTABLE} diff
        WORKING_DIRECTORY ${WORKING_DIR}
        OUTPUT_QUIET
        ERROR_QUIET
      )

      execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --abbrev=12 --dirty
        WORKING_DIRECTORY ${WORKING_DIR}
        OUTPUT_VARIABLE GIT_DESCRIPTION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
    endif()
  endif()
endif()

if(GIT_DESCRIPTION)
  set(NEWINFO "#define BUILD_GIT_DESCRIPTION \"${GIT_DESCRIPTION}\"")
else()
  set(NEWINFO "// No build information available")
endif()

# Only update the header if necessary.
if(NOT "${INFO}" STREQUAL "${NEWINFO}")
  file(WRITE ${BUILD_INFO_HEADER_PATH} "${NEWINFO}\n")
endif()
