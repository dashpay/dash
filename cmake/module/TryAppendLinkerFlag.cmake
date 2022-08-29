# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.18)
  include(CheckLinkerFlag)
endif()

function(try_append_linker_flag flags_var flag)
  string(MAKE_C_IDENTIFIER ${flag} result)
  string(TOUPPER ${result} result)
  set(result "LINKER_SUPPORTS${result}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
  if(COMMAND check_linker_flag)
    check_linker_flag(CXX ${flag} ${result})
  else()
    # Normalize locale during test compilation.
    set(locale_vars LC_ALL LC_MESSAGES LANG)
    foreach(v IN LISTS locale_vars)
      set(locale_vars_saved_${v} "$ENV{${v}}")
      set(ENV{${v}} C)
    endforeach()

    if(CMAKE_VERSION VERSION_LESS 3.14)
      set(CMAKE_REQUIRED_LIBRARIES "${flag}")
    else()
      set(CMAKE_REQUIRED_LINK_OPTIONS "${flag}")
    endif()
    include(CMakeCheckCompilerFlagCommonPatterns)
    check_compiler_flag_common_patterns(common_patterns)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("int main() { return 0; }" ${result} ${common_patterns})

    foreach(v IN LISTS locale_vars)
      set(ENV{${v}} ${locale_vars_saved_${v}})
    endforeach()
  endif()
  if(${result})
    string(STRIP "${${flags_var}} ${flag}" ${flags_var})
    set(${flags_var} "${${flags_var}}" PARENT_SCOPE)
  endif()
endfunction()
