# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# GCC 8.x (libstdc++) requires -lstdc++fs
# Clang 8.x (libc++) requires -lc++fs

function(check_std_filesystem)

  set(source "
#include <filesystem>

int main()
{
  (void)std::filesystem::current_path().root_name();
}
  ")

  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles("${source}" STD_FILESYSTEM_WORKS_WITHOUT_LINK)
  if(STD_FILESYSTEM_WORKS_WITHOUT_LINK)
    return()
  endif()

  add_library(std_filesystem INTERFACE)
  set(CMAKE_REQUIRED_LIBRARIES "stdc++fs")
  check_cxx_source_compiles("${source}" STD_FILESYSTEM_NEEDS_LINK_TO_STDCXXFS)
  if(STD_FILESYSTEM_NEEDS_LINK_TO_STDCXXFS)
    target_link_libraries(std_filesystem INTERFACE ${CMAKE_REQUIRED_LIBRARIES})
    return()
  endif()
  set(CMAKE_REQUIRED_LIBRARIES "c++fs")
  check_cxx_source_compiles("${source}" STD_FILESYSTEM_NEEDS_LINK_TO_CXXFS)
  if(STD_FILESYSTEM_NEEDS_LINK_TO_CXXFS)
    target_link_libraries(std_filesystem INTERFACE ${CMAKE_REQUIRED_LIBRARIES})
    return()
  endif()
  message(FATAL_ERROR "Cannot figure out how to use std::filesystem.")
endfunction()
