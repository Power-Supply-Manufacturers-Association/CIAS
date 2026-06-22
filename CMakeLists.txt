cmake_minimum_required(VERSION 3.15)
project(CIAS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj /Ox /W0")
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
else ()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

include(FetchContent)
message(STATUS "Fetching nlohmann/json")
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG tags/v3.11.3)
FetchContent_MakeAvailable(json)

# CIAS conversion library (consumed by downstream C++ repos, e.g. Kirchhoff):
#   CiasConverter        — CIAS <-> LTspice + the CiasCircuit struct
#   CiasCircuitConverter — CIAS atom-brick -> ngspice cards
add_library(CIAS STATIC
    src/CiasConverter.cpp
    src/CiasCircuitConverter.cpp)
target_include_directories(CIAS PUBLIC src)
target_link_libraries(CIAS PUBLIC nlohmann_json::nlohmann_json)

# PyCIAS — the pip/skbuild Python module. On by default so the package still
# builds standalone; downstream C++ consumers (Kirchhoff) set this OFF.
option(CIAS_BUILD_PYBIND "Build the PyCIAS Python module" ON)
if (CIAS_BUILD_PYBIND AND NOT EMSCRIPTEN)
    message(STATUS "Fetching pybind11 / pybind11_json")
    FetchContent_Declare(pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git)
    FetchContent_Declare(pybind11_json
        GIT_REPOSITORY https://github.com/pybind/pybind11_json.git)
    FetchContent_MakeAvailable(pybind11 pybind11_json)
    pybind11_add_module(PyCIAS src/bindings.cpp)
    target_include_directories(PyCIAS PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${pybind11_json_SOURCE_DIR}/include")
    target_link_libraries(PyCIAS PRIVATE CIAS)
    if (SKBUILD)
        install(TARGETS PyCIAS DESTINATION .)
    endif()
endif()

# Catch2 unit tests for the circuit converter.
option(CIAS_BUILD_TESTS "Build CIAS unit tests" ON)
if (NOT EMSCRIPTEN AND CIAS_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.8.1)
    FetchContent_MakeAvailable(Catch2)
    add_executable(cias_tests tests/test_cias_convert.cpp)
    target_link_libraries(cias_tests PRIVATE CIAS Catch2::Catch2WithMain)
    add_test(NAME cias_tests COMMAND cias_tests)
endif()
