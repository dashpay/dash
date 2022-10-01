# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

set(WITH_GUI "auto" CACHE STRING "Build GUI ([auto], Qt5, no)")
set(with_gui_values "auto" "Qt5" "no")
if(NOT WITH_GUI IN_LIST with_gui_values)
  message(FATAL_ERROR "WITH_GUI value is \"${WITH_GUI}\", but must be one of \"auto\", \"Qt5\" or \"no\".")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT WITH_GUI STREQUAL "no")
  if(CMAKE_VERSION VERSION_LESS 3.16)
    if(WITH_GUI STREQUAL "auto")
      message(WARNING "CMake >= 3.16 is required to build the GUI for macOS.\n"
                      "To skip this warning check, use \"-DWITH_GUI=no\".\n")
      set(WITH_GUI "no")
    else()
      message(FATAL_ERROR "CMake >= 3.16 is required to build the GUI for macOS.")
    endif()
  else()
    enable_language(OBJCXX)
  endif()
endif()

if(WITH_GUI STREQUAL "auto")
  set(bitcoin_qt_versions Qt5)
else()
  set(bitcoin_qt_versions ${WITH_GUI})
endif()

if(CMAKE_VERSION VERSION_LESS 3.12 AND CMAKE_CROSSCOMPILING)
  if(WITH_GUI STREQUAL "Qt5" OR bitcoin_qt_versions STREQUAL "Qt5")
    # Qt 5.15 uses `$<IN_LIST:>` in the `mkspecs/features/data/cmake/Qt5PluginTarget.cmake.in`.
    message(FATAL_ERROR "CMake >= 3.12 is required to build the GUI statically with Qt 5.")
  elseif(WITH_GUI STREQUAL "auto")
    message(WARNING "CMake >= 3.12 is required to build the GUI statically with Qt 5.\n"
                    "To skip this warning check, use \"-DWITH_GUI=no\".\n")
    set(WITH_GUI "no")
  endif()
endif()

if(NOT WITH_GUI STREQUAL "no")
  set(QT_NO_CREATE_VERSIONLESS_FUNCTIONS ON)
  set(QT_NO_CREATE_VERSIONLESS_TARGETS ON)

  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(COMMAND brew --prefix qt@5 OUTPUT_VARIABLE qt5_brew_prefix ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()

  if(WITH_GUI STREQUAL "auto")
    # The PATH_SUFFIXES option is required on OpenBSD systems.
    find_package(QT NAMES ${bitcoin_qt_versions}
      COMPONENTS Core
      HINTS ${qt5_brew_prefix}
      PATH_SUFFIXES Qt5
    )
    if(QT_FOUND)
      set(WITH_GUI Qt${QT_VERSION_MAJOR})
    else()
      message(WARNING "Qt not found, disabling.\n"
                      "To skip this warning check, use \"-DWITH_GUI=no\".\n")
      set(WITH_GUI "no")
    endif()
  endif()
endif()
