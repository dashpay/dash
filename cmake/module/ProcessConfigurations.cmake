# Copyright (c) 2023-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

include_guard(GLOBAL)

include(TryAppendCXXFlags)

macro(normalize_string string)
  string(REGEX REPLACE " +" " " ${string} "${${string}}")
  string(STRIP "${${string}}" ${string})
endmacro()

function(are_flags_overridden flags_var result_var)
  normalize_string(${flags_var})
  normalize_string(${flags_var}_INIT)
  if(${flags_var} STREQUAL ${flags_var}_INIT)
    set(${result_var} FALSE PARENT_SCOPE)
  else()
    set(${result_var} TRUE PARENT_SCOPE)
  endif()
endfunction()


# Removes duplicated flags. The relative order of flags is preserved.
# If duplicates are encountered, only the last instance is preserved.
function(deduplicate_flags flags)
  separate_arguments(${flags})
  list(REVERSE ${flags})
  list(REMOVE_DUPLICATES ${flags})
  list(REVERSE ${flags})
  list(JOIN ${flags} " " result)
  set(${flags} "${result}" PARENT_SCOPE)
endfunction()


function(get_all_configs output)
  get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(is_multi_config)
    set(all_configs ${CMAKE_CONFIGURATION_TYPES})
  else()
    get_property(all_configs CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS)
    if(NOT all_configs)
      # See https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html#default-and-custom-configurations
      set(all_configs Debug Release RelWithDebInfo MinSizeRel)
    endif()
  endif()
  set(${output} "${all_configs}" PARENT_SCOPE)
endfunction()


#[=[
Set the default build configuration.

See: https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html#build-configurations.

On single-configuration generators, this function sets the CMAKE_BUILD_TYPE variable to
the default build configuration, which can be overridden by the user at configure time if needed.

On multi-configuration generators, this function rearranges the CMAKE_CONFIGURATION_TYPES list,
ensuring that the default build configuration appears first while maintaining the order of the
remaining configurations. The user can choose a build configuration at build time.
]=]
function(set_default_config config)
  get_all_configs(all_configs)
  if(NOT ${config} IN_LIST all_configs)
    message(FATAL_ERROR "The default config is \"${config}\", but must be one of ${all_configs}.")
  endif()

  list(REMOVE_ITEM all_configs ${config})
  list(PREPEND all_configs ${config})

  get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(is_multi_config)
    get_property(help_string CACHE CMAKE_CONFIGURATION_TYPES PROPERTY HELPSTRING)
    set(CMAKE_CONFIGURATION_TYPES "${all_configs}" CACHE STRING "${help_string}" FORCE)
    # Also see https://gitlab.kitware.com/cmake/cmake/-/issues/19512.
    set(CMAKE_TRY_COMPILE_CONFIGURATION "${config}" PARENT_SCOPE)
  else()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
      STRINGS "${all_configs}"
    )
    if(NOT CMAKE_BUILD_TYPE)
      message(STATUS "Setting build type to \"${config}\" as none was specified")
      get_property(help_string CACHE CMAKE_BUILD_TYPE PROPERTY HELPSTRING)
      set(CMAKE_BUILD_TYPE "${config}" CACHE STRING "${help_string}" FORCE)
    endif()
    set(CMAKE_TRY_COMPILE_CONFIGURATION "${CMAKE_BUILD_TYPE}" PARENT_SCOPE)
  endif()
endfunction()

function(remove_cxx_flag_from_all_configs flag)
  get_all_configs(all_configs)
  foreach(config IN LISTS all_configs)
    string(TOUPPER "${config}" config_uppercase)
    set(flags "${CMAKE_CXX_FLAGS_${config_uppercase}}")
    separate_arguments(flags)
    list(FILTER flags EXCLUDE REGEX "${flag}")
    list(JOIN flags " " new_flags)
    set(CMAKE_CXX_FLAGS_${config_uppercase} "${new_flags}" PARENT_SCOPE)
    set(CMAKE_CXX_FLAGS_${config_uppercase} "${new_flags}"
      CACHE STRING
      "Flags used by the CXX compiler during ${config_uppercase} builds."
      FORCE
    )
  endforeach()
endfunction()

function(append_flag_to_config lang config flag)
  string(TOUPPER "${config}" config_uppercase)
  set(flags_var CMAKE_${lang}_FLAGS_${config_uppercase})
  set(flags "${${flags_var}}")
  separate_arguments(flags)
  # Appending unconditionally would re-append on every reconfigure, because the
  # value read back here is the one this function forced into the cache last time.
  if(NOT "${flag}" IN_LIST flags)
    list(APPEND flags "${flag}")
  endif()
  list(JOIN flags " " new_flags)
  set(${flags_var} "${new_flags}" PARENT_SCOPE)
  set(${flags_var} "${new_flags}"
    CACHE STRING
    "Flags used by the ${lang} compiler during ${config_uppercase} builds."
    FORCE
  )
endfunction()

function(replace_cxx_flag_in_config config old_flag new_flag)
  string(TOUPPER "${config}" config_uppercase)
  string(REGEX REPLACE "(^| )${old_flag}( |$)" "\\1${new_flag}\\2" new_flags "${CMAKE_CXX_FLAGS_${config_uppercase}}")
  set(CMAKE_CXX_FLAGS_${config_uppercase} "${new_flags}" PARENT_SCOPE)
  set(CMAKE_CXX_FLAGS_${config_uppercase} "${new_flags}"
    CACHE STRING
    "Flags used by the CXX compiler during ${config_uppercase} builds."
    FORCE
  )
endfunction()

set_default_config(RelWithDebInfo)

# Redefine/adjust per-configuration flags.
# Keep this list in sync with the --enable-debug DEBUG_CPPFLAGS in configure.ac.
# Dash uses DEBUG_CORE, not upstream's DEBUG, which is what span.h and
# init/common.cpp check.
target_compile_definitions(core_interface_debug INTERFACE
  DEBUG_CORE
  DEBUG_LOCKORDER
  DEBUG_LOCKCONTENTION
  RPC_DOC_CHECK
  ABORT_ON_FAILED_ASSUME
  BOOST_MULTI_INDEX_ENABLE_SAFE_MODE
)
# We leave assertions on.
if(MSVC)
  remove_cxx_flag_from_all_configs(/DNDEBUG)
else()
  remove_cxx_flag_from_all_configs(-DNDEBUG)

  # Adjust flags used by the CXX compiler during RELEASE builds.
  # Prefer -O2 optimization level. (-O3 is CMake's default for Release for many compilers.)
  replace_cxx_flag_in_config(Release -O3 -O2)

  # Autotools always keeps at least -g1 on non-debug builds so libbacktrace can
  # still symbolize a crash, with -fno-omit-frame-pointer to keep the result
  # usable under optimization; see the non-debug branch in configure.ac, which
  # sets both DEBUG_CFLAGS and DEBUG_CXXFLAGS. These belong on the configuration
  # rather than on stacktraces_interface: flags carried on a target are appended
  # after the per-configuration ones, so a debug level set there would clamp
  # every configuration to it. RelWithDebInfo already carries -g and only needs
  # the frame pointer.
  include(CheckCCompilerFlag)
  try_append_cxx_flags("-g1" RESULT_VAR cxx_supports_g1)
  check_c_compiler_flag("-g1" C_SUPPORTS_G1)
  foreach(config IN ITEMS Release MinSizeRel)
    if(cxx_supports_g1)
      append_flag_to_config(CXX ${config} -g1)
    endif()
    if(C_SUPPORTS_G1)
      append_flag_to_config(C ${config} -g1)
    endif()
  endforeach()
  unset(cxx_supports_g1)

  try_append_cxx_flags("-fno-omit-frame-pointer" RESULT_VAR cxx_supports_no_omit_fp)
  check_c_compiler_flag("-fno-omit-frame-pointer" C_SUPPORTS_NO_OMIT_FP)
  foreach(config IN ITEMS Release RelWithDebInfo MinSizeRel)
    if(cxx_supports_no_omit_fp)
      append_flag_to_config(CXX ${config} -fno-omit-frame-pointer)
    endif()
    if(C_SUPPORTS_NO_OMIT_FP)
      append_flag_to_config(C ${config} -fno-omit-frame-pointer)
    endif()
  endforeach()
  unset(cxx_supports_no_omit_fp)

  are_flags_overridden(CMAKE_CXX_FLAGS_DEBUG cxx_flags_debug_overridden)
  if(NOT cxx_flags_debug_overridden)
    # Redefine flags used by the CXX compiler during DEBUG builds.
    try_append_cxx_flags("-g3" RESULT_VAR compiler_supports_g3)
    if(compiler_supports_g3)
      replace_cxx_flag_in_config(Debug -g -g3)
    endif()
    unset(compiler_supports_g3)

    try_append_cxx_flags("-ftrapv" RESULT_VAR compiler_supports_ftrapv)
    if(compiler_supports_ftrapv)
      string(PREPEND CMAKE_CXX_FLAGS_DEBUG "-ftrapv ")
    endif()
    unset(compiler_supports_ftrapv)

    string(PREPEND CMAKE_CXX_FLAGS_DEBUG "-O0 ")

    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}"
      CACHE STRING
      "Flags used by the CXX compiler during DEBUG builds."
      FORCE
    )
  endif()
  unset(cxx_flags_debug_overridden)
endif()

set(CMAKE_CXX_FLAGS_COVERAGE "-Og --coverage")
set(CMAKE_OBJCXX_FLAGS_COVERAGE "-Og --coverage")
set(CMAKE_EXE_LINKER_FLAGS_COVERAGE "--coverage")
set(CMAKE_SHARED_LINKER_FLAGS_COVERAGE "--coverage")
get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(is_multi_config)
  if(NOT "Coverage" IN_LIST CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES Coverage)
  endif()
endif()
