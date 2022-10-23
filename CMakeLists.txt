# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# We are able to build on Ubuntu Bionic.
# On the other hand, some useful features lack:
#  - 3.11: add_library() and add_executable() commands can now be called without any sources
#          and will not complain as long as sources are added later via the target_sources()
#          command.
#  - 3.11: The target_*() commands learned to set the INTERFACE properties on Imported Targets.
#  - 3.12: The add_compile_definitions() command was added to set preprocessor definitions
#          at directory level. This supersedes add_definitions().
#  - 3.12: If the CONFIGURE_DEPENDS flag is specified, CMake will add logic to the main
#          build system check target to rerun the flagged GLOB commands at build time.
#  - 3.12: Object libraries can be linked to with target_link_libraries().
#  - 3.12: A new $<TARGET_EXISTS:...> generator expression has been added.
#  - 3.13: The target_link_options() command was created to specify link options for targets
#          and their dependents.
#  - 3.13: The target_link_libraries() command may now be called to modify targets created
#          outside the current directory.
#  - 3.14: The file() command learned a new sub-command, CREATE_LINK, which can be used to
#          create hard or symbolic links.
#  - 3.14: The FindSQLite3 module was added to find the SQLite v3.x library.
#  - 3.15: The CMAKE_MSVC_RUNTIME_LIBRARY variable and MSVC_RUNTIME_LIBRARY target property were
#          introduced to select the runtime library used by compilers targeting the MSVC ABI.
#  - 3.15: A new imported target Boost::headers is now defined (same as Boost::boost).
#  - 3.18: The add_library() and add_executable() commands learned to create Alias Targets
#          referencing non-GLOBAL Imported Targets.
#  - 3.18: The CheckLinkerFlag module has been added to provide a facility to check validity
#          of link flags.
cmake_minimum_required(VERSION 3.10)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.15)
  # MSVC runtime library flags are selected by the CMAKE_MSVC_RUNTIME_LIBRARY abstraction.
  cmake_policy(SET CMP0091 NEW)
  # MSVC warning flags are not in CMAKE_<LANG>_FLAGS by default.
  cmake_policy(SET CMP0092 NEW)
endif()
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.20)
  # MSVC RTTI flag /GR is not added to CMAKE_CXX_FLAGS by default.
  cmake_policy(SET CMP0117 NEW)
endif()

project("Bitcoin Core"
  VERSION 23.0.0
  DESCRIPTION "Bitcoin client software"
  LANGUAGES CXX C ASM
)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/module)

# Configurable options.
# When adding a new option, end the <help_text> with a full stop for consistency.
option(CXX20 "Enable compilation in c++20 mode." OFF)
option(HARDENING "Attempt to harden the resulting executables." ON)

if(CXX20 OR MSVC)
  set(CMAKE_CXX_STANDARD 20)
else()
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(TryAppendCompilerFlag)
include(TryAppendLinkerFlag)

# Redefine "Debug" configuration flags.
set(debug_cpp_flags "-DDEBUG -DDEBUG_LOCKORDER -DDEBUG_LOCKCONTENTION -DRPC_DOC_CHECK -DABORT_ON_FAILED_ASSUME")
if(MSVC)
  set(CMAKE_C_FLAGS_DEBUG "${debug_cpp_flags} /Zi /Ob0 /Od /RTC1")
  set(CMAKE_CXX_FLAGS_DEBUG "${debug_cpp_flags} /Zi /Ob0 /Od /RTC1")
else()
  set(debug_c_flags "")
  try_append_cflag(debug_c_flags "-O0")
  try_append_cflag(debug_c_flags "-g3")
  if(NOT C_SUPPORTS_G3)
    try_append_cflag(debug_c_flags "-g")
  endif()
  set(CMAKE_C_FLAGS_DEBUG "${debug_cpp_flags} ${debug_c_flags}")
  set(debug_cxx_flags "")
  try_append_cxxflag(debug_cxx_flags "-O0")
  try_append_cxxflag(debug_cxx_flags "-g3")
  if(NOT CXX_SUPPORTS_G3)
    try_append_cxxflag(debug_cxx_flags "-g")
  endif()
  try_append_cxxflag(debug_cxx_flags "-ftrapv")
  if(MINGW)
    try_append_cxxflag(debug_cxx_flags "-Wa,-mbig-obj")
  endif()
  set(CMAKE_CXX_FLAGS_DEBUG "${debug_cpp_flags} ${debug_cxx_flags}")
endif()

# Redefine "Release" configuration flags.
if(MSVC)
  set(CMAKE_C_FLAGS_RELEASE "/O2 /Ob2")
  set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2")
else()
  set(release_c_flags "")
  try_append_cflag(release_c_flags "-O2")
  try_append_cflag(release_c_flags "-g")
  set(CMAKE_C_FLAGS_RELEASE "${release_c_flags}")
  set(release_cxx_flags "")
  try_append_cxxflag(release_cxx_flags "-O2")
  try_append_cxxflag(release_cxx_flags "-g")
  set(CMAKE_CXX_FLAGS_RELEASE "${release_cxx_flags}")
endif()

set(default_build_type "Release")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to \"${default_build_type}\" as none was specified")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE STRING "Choose the type of build." FORCE)
endif()

set(hardened_cxx_flags "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # All versions of gcc that we commonly use for building are subject to bug
  # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90348. To work around that, set
  # -fstack-reuse=none for all gcc builds.
  try_append_cxxflag(hardened_cxx_flags "-fstack-reuse=none")
endif()
if(HARDENING)
  if(MSVC)
    add_link_options(/DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)
  else()
    try_append_cxxflag(hardened_cxx_flags "-Wstack-protector")
    try_append_cxxflag(hardened_cxx_flags "-fstack-protector-all")
    try_append_cxxflag(hardened_cxx_flags "-fcf-protection=full")
    if(MINGW)
      link_libraries(ssp)
    else()
      # stack-clash-protection doesn't currently work, and likely should just be skipped for Windows.
      # See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90458 for more details.
      try_append_cxxflag(hardened_cxx_flags "-fstack-clash-protection")
    endif()

    set(hardened_linker_flags "")
    try_append_linker_flag(hardened_linker_flags "-Wl,--enable-reloc-section")
    try_append_linker_flag(hardened_linker_flags "-Wl,--dynamicbase")
    try_append_linker_flag(hardened_linker_flags "-Wl,--nxcompat")
    try_append_linker_flag(hardened_linker_flags "-Wl,--high-entropy-va")
    try_append_linker_flag(hardened_linker_flags "-Wl,-z,relro")
    try_append_linker_flag(hardened_linker_flags "-Wl,-z,now")
    try_append_linker_flag(hardened_linker_flags "-Wl,-z,separate-code")
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      try_append_linker_flag(hardened_linker_flags "-Wl,-bind_at_load")
    endif()
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.13)
      separate_arguments(hardened_linker_flags)
      add_link_options(${hardened_linker_flags})
    else()
      link_libraries(${hardened_linker_flags})
    endif()
  endif()
endif()
separate_arguments(hardened_cxx_flags)
add_compile_options(${hardened_cxx_flags})

if(WIN32)
  add_definitions(-D_WIN32_WINNT=0x0601 -D_WIN32_IE=0x0501 -DWIN32_LEAN_AND_MEAN)
  if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    add_definitions(/DNOMINMAX /D_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/utf-8 /Zc:__cplusplus /W3 /wd4018 /wd4244 /wd4267 /wd4715 /wd4805)
    add_link_options(/NODEFAULTLIB:LIBCMTD)
    add_link_options(/LTCG:OFF)
  elseif(MINGW)
    add_definitions(-D_MT -DWIN32 -D_WINDOWS)
    set(mingw_linker_flags "")
    try_append_linker_flag(mingw_linker_flags "-static")
    # We require Windows 7 (NT 6.1) or later.
    try_append_linker_flag(mingw_linker_flags "-Wl,--major-subsystem-version,6")
    try_append_linker_flag(mingw_linker_flags "-Wl,--minor-subsystem-version,1")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.13)
      separate_arguments(mingw_linker_flags)
      add_link_options(${mingw_linker_flags})
    else()
      link_libraries(${mingw_linker_flags})
    endif()
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  add_definitions(-DMAC_OSX -DOBJC_OLD_DISPATCH_PROTOTYPES=0)
  # These flags are specific to ld64, and may cause issues with other linkers.
  # For example: GNU ld will interpret -dead_strip as -de and then try and use
  # "ad_strip" as the symbol for the entry point.
  try_append_linker_flag(darwin_linker_flags "-Wl,-dead_strip")
  try_append_linker_flag(darwin_linker_flags "-Wl,-dead_strip_dylibs")
  try_append_linker_flag(darwin_linker_flags "-Wl,-headerpad_max_install_names")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.13)
    separate_arguments(darwin_linker_flags)
    add_link_options(${darwin_linker_flags})
  else()
    link_libraries(${darwin_linker_flags})
  endif()
endif()

message("\n")
message("Configure summary")
message("=================")
get_directory_property(definitions COMPILE_DEFINITIONS)
message("Preprocessor defined macros ........... ${definitions}")
message("C compiler ............................ ${CMAKE_C_COMPILER}")
message("CFLAGS ................................ ${CMAKE_C_FLAGS}")
message("C++ compiler .......................... ${CMAKE_CXX_COMPILER}")
message("CXXFLAGS .............................. ${CMAKE_CXX_FLAGS}")
get_directory_property(common_compile_options COMPILE_OPTIONS)
string(REPLACE ";" " " common_compile_options "${common_compile_options}")
message("Common compile options ................ ${common_compile_options}")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.13)
  get_directory_property(common_link_options LINK_OPTIONS)
  string(REPLACE ";" " " common_link_options "${common_link_options}")
  message("Common link options ................... ${common_link_options}")
endif()
if(DEFINED CMAKE_BUILD_TYPE)
  message("Build type:")
  message(" - configuration ...................... ${CMAKE_BUILD_TYPE}")
  string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
  message(" - CFLAGS ............................. ${CMAKE_C_FLAGS_${build_type}}")
  message(" - CXXFLAGS ........................... ${CMAKE_CXX_FLAGS_${build_type}}")
  message(" - LDFLAGS for executables ............ ${CMAKE_EXE_LINKER_FLAGS_${build_type}}")
  message(" - LDFLAGS for shared libraries ....... ${CMAKE_SHARED_LINKER_FLAGS_${build_type}}")
else()
  message("Available configurations .............. ${CMAKE_CONFIGURATION_TYPES}")
  message("Debug configuration:")
  message(" - CFLAGS ............................. ${CMAKE_C_FLAGS_DEBUG}")
  message(" - CXXFLAGS ........................... ${CMAKE_CXX_FLAGS_DEBUG}")
  message(" - LDFLAGS for executables ............ ${CMAKE_EXE_LINKER_FLAGS_DEBUG}")
  message(" - LDFLAGS for shared libraries ....... ${CMAKE_SHARED_LINKER_FLAGS_DEBUG}")
  message("Release configuration:")
  message(" - CFLAGS ............................. ${CMAKE_C_FLAGS_RELEASE}")
  message(" - CXXFLAGS ........................... ${CMAKE_CXX_FLAGS_RELEASE}")
  message(" - LDFLAGS for executables ............ ${CMAKE_EXE_LINKER_FLAGS_RELEASE}")
  message(" - LDFLAGS for shared libraries ....... ${CMAKE_SHARED_LINKER_FLAGS_RELEASE}")
endif()
message("\n")
