file(GLOB HEADERS ${PROJECT_SOURCE_DIR}/include/dashbls/*.hpp)
source_group("SrcHeaders" FILES ${HEADERS})

list(APPEND bls_sources
  ${HEADERS}
  privatekey.cpp
  bls.cpp
  chaincode.cpp
  elements.cpp
  extendedprivatekey.cpp
  extendedpublickey.cpp
  legacy.cpp
  schemes.cpp
  threshold.cpp
  ${blst_SOURCE_DIR}/src/server.c
)

if(MSVC)
  list(APPEND bls_sources
    ${blst_SOURCE_DIR}/build/win64/add_mod_256-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/add_mod_384-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/add_mod_384x384-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/ct_inverse_mod_256-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/ct_is_square_mod_384-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/ctq_inverse_mod_384-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/ctx_inverse_mod_384-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/div3w-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/mulq_mont_256-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/mulq_mont_384-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/mulx_mont_256-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/mulx_mont_384-x86_64.asm
    ${blst_SOURCE_DIR}/build/win64/sha256-x86_64.asm
  )
else()
  list(APPEND bls_sources
    ${blst_SOURCE_DIR}/build/assembly.S
  )
  set_source_files_properties(${blst_SOURCE_DIR}/src/server.c PROPERTIES COMPILE_OPTIONS -fno-builtin)
  add_compile_options(-fPIC)
  add_compile_options(-Wall)
  add_compile_options(-Wextra)
  if((CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)") AND (NOT EMSCRIPTEN))
    add_compile_options(-mno-avx)
  endif()
endif()

add_library(dashbls ${bls_sources})
target_include_directories(dashbls PUBLIC ${blst_SOURCE_DIR}/bindings ${PROJECT_SOURCE_DIR}/include/dashbls/)
target_compile_definitions(dashbls PRIVATE __BLST_PORTABLE__ BLSALLOC_MIMALLOC=1)
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|aarch64|arm64|ARM64")
  # blst ships assembly for x86_64 and aarch64 only; other 64-bit targets
  # (riscv64, powerpc64, ...) build its portable C code instead.
  target_compile_definitions(dashbls PRIVATE __BLST_NO_ASM__)
endif()
target_link_libraries(dashbls
  PUBLIC
    mimalloc-static
)

install(FILES ${blst_SOURCE_DIR}/bindings/blst.h ${blst_SOURCE_DIR}/bindings/blst_aux.h DESTINATION include/dashbls)
install(FILES ${HEADERS} DESTINATION include/dashbls)
install(FILES $<TARGET_FILE:dashbls> DESTINATION lib)

if(BUILD_BLS_TESTS)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../depends/catch2/CMakeLists.txt)

  add_executable(runtest test.cpp)
  if(EMSCRIPTEN)
    target_link_options(runtest PRIVATE "-sEXPORTED_FUNCTIONS=_malloc")
  endif()
  target_link_libraries(runtest
    PRIVATE
    dashbls
    catch2)

  if(EMSCRIPTEN)
    target_link_options(runtest PRIVATE "SHELL:-s MALLOC=emmalloc")
    if(CMAKE_BUILD_TYPE MATCHES "Debug")
      target_link_options(runtest PRIVATE "SHELL:-s ASSERTIONS=2")
    endif()
  endif()
endif()

if(BUILD_BLS_BENCHMARKS)
  add_executable(runbench test-bench.cpp)
  target_link_libraries(runbench PRIVATE dashbls)

  if(EMSCRIPTEN)
    target_link_options(runbench PRIVATE "SHELL:-s MALLOC=emmalloc")
    if(CMAKE_BUILD_TYPE MATCHES "Debug")
      target_link_options(runbench PRIVATE "SHELL:-s ASSERTIONS=2")
    endif()
  endif()
endif()

if(BUILD_BLS_TESTS)
  # runtest compares the library against test-vectors/reference.txt;
  # vectorgen regenerates that file from whichever library it is built against.
  target_compile_definitions(runtest PRIVATE
    DASHBLS_REFERENCE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../test-vectors/reference.txt")

  add_executable(vectorgen ${CMAKE_CURRENT_SOURCE_DIR}/../tools/vectorgen.cpp)
  target_include_directories(vectorgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(vectorgen PRIVATE dashbls)

  if(EMSCRIPTEN)
    # mimalloc's Emscripten backend is built on emmalloc
    target_link_options(vectorgen PRIVATE "SHELL:-s MALLOC=emmalloc")
  endif()
endif()
