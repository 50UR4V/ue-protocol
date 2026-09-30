# UltraEdge companion protocol — standalone build (library + tests + demo).
# Embedded-safe C++11; no external dependencies; builds on any host toolchain.
#
#   cmake -B build . && cmake --build build && ctest --test-dir build
#
# The library target (ultraedge_protocol) is also intended to be added to the
# EdgeTX firmware build via add_subdirectory / source inclusion (see firmware/).
cmake_minimum_required(VERSION 3.14)
project(ultraedge_protocol CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ultraedge_protocol STATIC src/protocol.cpp)
target_include_directories(ultraedge_protocol PUBLIC include)
target_compile_options(ultraedge_protocol PRIVATE -Wall -Wextra)

enable_testing()

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE ultraedge_protocol)
add_test(NAME protocol_unit_tests COMMAND test_protocol)

add_executable(loopback_demo demo/loopback_demo.cpp)
target_link_libraries(loopback_demo PRIVATE ultraedge_protocol)
add_test(NAME loopback_demo COMMAND loopback_demo)
