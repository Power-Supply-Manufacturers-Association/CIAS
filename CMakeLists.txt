cmake_minimum_required(VERSION 3.15)
project(PyCIAS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Ox")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W0")
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
else ()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

include(FetchContent)

message(STATUS "Fetching nlohmann/json")
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG tags/v3.11.3)
FetchContent_MakeAvailable(json)
include_directories("${CMAKE_BINARY_DIR}/_deps/json-src/include/")

message(STATUS "Fetching pybind11")
FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git)

message(STATUS "Fetching pybind11_json")
FetchContent_Declare(pybind11_json
    GIT_REPOSITORY https://github.com/pybind/pybind11_json.git)

FetchContent_MakeAvailable(pybind11 pybind11_json)
include_directories("${CMAKE_BINARY_DIR}/_deps/pybind11-src/include/")
include_directories("${CMAKE_BINARY_DIR}/_deps/pybind11_json-src/include/")

pybind11_add_module(PyCIAS
    src/bindings.cpp
    src/CiasConverter.cpp
)

target_include_directories(PyCIAS PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if (SKBUILD)
    install(TARGETS PyCIAS DESTINATION .)
endif()
