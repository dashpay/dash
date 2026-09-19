file(GLOB HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
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
  privatekey.cpp
  bls.cpp
  elements.cpp
  schemes.cpp
  ${blst_SOURCE_DIR}/src/server.c
)

  list(APPEND bls_sources
    ${blst_SOURCE_DIR}/build/assembly.S
  )
  add_compile_options(-fno-builtin)
  add_compile_options(-fPIC)
  add_compile_options(-Wall)
  add_compile_options(-Wextra)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    add_compile_options(-mno-avx)
  endif()


add_library(dashbls ${bls_sources})
target_include_directories(dashbls PUBLIC ${blst_SOURCE_DIR}/bindings ${PROJECT_SOURCE_DIR}/include/dashbls/)
target_compile_definitions(dashbls PRIVATE __BLST_PORTABLE__ BLSALLOC_MIMALLOC=1)
target_link_libraries(dashbls
  PUBLIC
    mimalloc-static
)

install(DIRECTORY ${blst_SOURCE_DIR}/include/ DESTINATION include/dashbls)
install(DIRECTORY ${blst_BINARY_DIR}/include/ DESTINATION include/dashbls)
install(FILES ${HEADERS} DESTINATION include/dashbls)
install(FILES $<TARGET_FILE:dashbls> DESTINATION lib)

if(BUILD_BLS_TESTS)
  include(${CMAKE_CURRENT_SOURCE_DIR}/../depends/catch2/CMakeLists.txt)

  add_executable(runtest test.cpp)
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
endif()
