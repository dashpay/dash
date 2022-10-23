# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

find_package(PkgConfig REQUIRED)

macro(cross_pkg_check_modules)
  if(CMAKE_CROSSCOMPILING)
    set(pkg_config_path_saved "$ENV{PKG_CONFIG_PATH}")
    set(pkg_config_libdir_saved "$ENV{PKG_CONFIG_LIBDIR}")
    set(ENV{PKG_CONFIG_PATH} ${PKG_CONFIG_PATH})
    set(ENV{PKG_CONFIG_LIBDIR} ${PKG_CONFIG_LIBDIR})
    pkg_check_modules(${ARGV})
    set(ENV{PKG_CONFIG_PATH} ${pkg_config_path_saved})
    set(ENV{PKG_CONFIG_LIBDIR} ${pkg_config_libdir_saved})
  else()
    pkg_check_modules(${ARGV})
  endif()
endmacro()
