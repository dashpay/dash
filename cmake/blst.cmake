# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Mirrors blst's build.sh: server.c #includes the rest of the C sources, and
# build/assembly.S includes the assembly for the target. __BLST_PORTABLE__
# picks ADX at runtime rather than from the build machine's CPU.

include(CheckCSourceCompiles)

set(BLST_USE_ASM OFF)
if(ENABLE_BLST_ASM)
  check_c_source_compiles("
    #if !defined(__x86_64__) && !defined(__aarch64__)
    #error \"blst has no assembly for this target\"
    #endif
    int main(void) { return 0; }
    " BLST_TARGET_HAS_ASM
  )
  set(BLST_USE_ASM ${BLST_TARGET_HAS_ASM})
endif()

add_library(blst STATIC EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/src/blst/src/server.c
)
if(BLST_USE_ASM)
  # Compiled as C so that it gets the target flags that the depends
  # toolchain puts in the C compiler and its flags, which CMake's ASM language
  # would not pick up. The later -x overrides the -x c that CMake adds for an
  # explicit LANGUAGE.
  set(blst_asm ${PROJECT_SOURCE_DIR}/src/blst/build/assembly.S)
  set_source_files_properties(${blst_asm} PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-x;assembler-with-cpp"
  )
  target_sources(blst PRIVATE ${blst_asm})
else()
  target_compile_definitions(blst PRIVATE __BLST_NO_ASM__)
endif()
target_compile_definitions(blst PRIVATE __BLST_PORTABLE__)
target_compile_options(blst PRIVATE -fno-builtin)
target_include_directories(blst SYSTEM
  INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/blst/bindings>
)
target_link_libraries(blst PRIVATE core_interface)
set_target_properties(blst PROPERTIES
  EXPORT_COMPILE_COMMANDS OFF
)
