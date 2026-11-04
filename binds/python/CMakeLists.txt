find_package(pybind11 2.13.6 CONFIG QUIET)

if(NOT pybind11_FOUND)
  message(STATUS "pybind11 not found locally; fetching it")
  include(FetchContent)
  FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
  )
  FetchContent_MakeAvailable(pybind11)
endif()

pybind11_add_module(dashbls_py ${CMAKE_CURRENT_SOURCE_DIR}/pythonbindings.cpp)
set_target_properties(dashbls_py PROPERTIES OUTPUT_NAME dashbls)
target_include_directories(dashbls_py PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)
target_link_libraries(dashbls_py PRIVATE dashbls)

if(VERSION_INFO)
  target_compile_definitions(dashbls_py PRIVATE VERSION_INFO="${VERSION_INFO}")
endif()
