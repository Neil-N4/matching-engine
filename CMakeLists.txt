cmake_minimum_required(VERSION 3.20)

project(matching_engine
    VERSION 0.1.0
    DESCRIPTION "C++20 L3 matching engine and microstructure alpha pipeline"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MATCHING_ENGINE_NATIVE "Enable -march=native when supported" ON)
option(MATCHING_ENGINE_TESTS "Build unit tests" ON)
option(MATCHING_ENGINE_GTEST "Build GoogleTest coverage when GTest is available" ON)
option(MATCHING_ENGINE_REQUIRE_GTEST "Fail configure if GoogleTest coverage is enabled but unavailable" OFF)
option(MATCHING_ENGINE_BENCHMARKS "Build benchmark target" ON)

add_library(matching_engine_headers INTERFACE)
target_include_directories(matching_engine_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(matching_engine_headers INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-exceptions>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fno-rtti>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>)

if(MATCHING_ENGINE_NATIVE)
    target_compile_options(matching_engine_headers INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-march=native>)
endif()

add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE matching_engine_headers)

if(UNIX AND NOT APPLE)
    target_link_libraries(matching_engine PRIVATE pthread)
endif()

if(MATCHING_ENGINE_TESTS)
    enable_testing()
    add_executable(matching_engine_tests tests/unit_tests.cpp)
    target_link_libraries(matching_engine_tests PRIVATE matching_engine_headers)
    add_test(NAME matching_engine_tests COMMAND matching_engine_tests)

    if(MATCHING_ENGINE_GTEST)
        find_package(GTest QUIET)
        if(GTest_FOUND)
            include(GoogleTest)
            add_executable(matching_engine_gtests tests/gtest_tests.cpp)
            target_link_libraries(matching_engine_gtests PRIVATE
                matching_engine_headers
                GTest::gtest_main)
            gtest_discover_tests(matching_engine_gtests)
        elseif(MATCHING_ENGINE_REQUIRE_GTEST)
            message(FATAL_ERROR "MATCHING_ENGINE_REQUIRE_GTEST is ON but GTest was not found")
        else()
            message(STATUS "GTest not found; skipping matching_engine_gtests")
        endif()
    endif()
endif()

if(MATCHING_ENGINE_BENCHMARKS)
    add_executable(engine_bench benchmarks/engine_bench.cpp)
    target_link_libraries(engine_bench PRIVATE matching_engine_headers)

    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        target_compile_definitions(engine_bench PRIVATE MATCHING_ENGINE_WITH_GOOGLE_BENCHMARK=1)
        target_link_libraries(engine_bench PRIVATE benchmark::benchmark)
    endif()
endif()
