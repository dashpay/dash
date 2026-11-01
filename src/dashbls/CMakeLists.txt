CMAKE_MINIMUM_REQUIRED(VERSION 3.18.0 FATAL_ERROR)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release"
    CACHE STRING "Possible values are empty, Debug, Release, RelWithDebInfo, MinSizeRel, ..."
    FORCE
  )
endif()

project(BLS)

if(MSVC)
  enable_language(ASM_MASM)
else()
  enable_language(ASM)
endif()

set(BUILD_BLS_JS_BINDINGS "1" CACHE STRING "")
set(BUILD_BLS_PYTHON_BINDINGS "1" CACHE STRING "")
set(BUILD_BLS_TESTS "1" CACHE STRING "")
set(BUILD_BLS_BENCHMARKS "1" CACHE STRING "")

message(STATUS "Build python bindings: ${BUILD_BLS_PYTHON_BINDINGS}")
message(STATUS "Build tests: ${BUILD_BLS_TESTS}")
message(STATUS "Build benchmarks: ${BUILD_BLS_BENCHMARKS}")

# Add path for custom modules
set(CMAKE_MODULE_PATH
  ${CMAKE_MODULE_PATH}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules
)

# Disable mimalloc shared libraries and tests, enable secure mode
set(MI_SECURE       "on" CACHE STRING "")

set(MI_BUILD_SHARED "off" CACHE STRING "")
set(MI_BUILD_TESTS  "off" CACHE STRING "")
set(MI_OVERRIDE     "off" CACHE STRING "")
add_subdirectory(depends/mimalloc)

include(FetchContent)

set(BLST_GIT_TAG "a8cd361c9f671577aeab3f074098443af92a53fc")
set(BLST_REPOSITORY "https://github.com/supranational/blst")

message(STATUS "blst will be built from: ${BLST_GIT_TAG} and repository ${BLST_REPOSITORY}")

FetchContent_Declare(
  blst
  GIT_REPOSITORY ${BLST_REPOSITORY}
  GIT_TAG ${BLST_GIT_TAG}
)
FetchContent_MakeAvailable(blst)

add_subdirectory(src)


# Write include paths for rust binding
if(EMSCRIPTEN)
  file(APPEND "${CMAKE_CURRENT_LIST_DIR}/build/include_paths.txt" "${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}/c++/v1/;")
endif()

file(APPEND "${CMAKE_CURRENT_LIST_DIR}/build/include_paths.txt" "${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES};")

file(APPEND "${CMAKE_CURRENT_LIST_DIR}/build/include_paths.txt" "${blst_SOURCE_DIR}/bindings;")

if(EMSCRIPTEN)
  if(BUILD_BLS_JS_BINDINGS)
    add_subdirectory(js-bindings)
  endif()
else()
  # emscripten can't build python bindings, it produces only javascript
  # add_subdirectory(contrib/pybind11)
  if(BUILD_BLS_PYTHON_BINDINGS)
    add_subdirectory(binds/python)
  endif()
endif()
