##############################################################################
# HFT NanoTick - High-Frequency Trading System
# CMake Build Configuration
##############################################################################

cmake_minimum_required(VERSION 3.20)

project(HFT_NanoTick
    VERSION 1.0.0
    DESCRIPTION "Ultra-low latency HFT trading system skeleton"
    LANGUAGES CXX
)

##############################################################################
# Project Options
##############################################################################

option(HFT_BUILD_TESTS "Build unit tests" ON)
option(HFT_BUILD_BENCHMARKS "Build benchmarks" ON)
option(HFT_ENABLE_SANITIZERS "Enable address/undefined sanitizers" OFF)
option(HFT_ENABLE_LTO "Enable Link-Time Optimization" OFF)
option(HFT_NATIVE_ARCH "Optimize for native CPU architecture" ON)

##############################################################################
# C++ Standard Configuration
##############################################################################

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

##############################################################################
# Compiler Detection and Flags
##############################################################################

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(HFT_WARNING_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wcast-align
        -Wunused
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
    )

    set(HFT_OPTIMIZATION_FLAGS
        -O3
        -ffast-math
        -funroll-loops
        -fno-omit-frame-pointer
    )

    if(HFT_NATIVE_ARCH)
        list(APPEND HFT_OPTIMIZATION_FLAGS -march=native)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        list(APPEND HFT_WARNING_FLAGS
            -Wlogical-op
            -Wduplicated-cond
            -Wduplicated-branches
        )
    endif()

elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(HFT_WARNING_FLAGS
        /W4
        /permissive-
    )

    set(HFT_OPTIMIZATION_FLAGS
        /O2
        /Ob3
        /GL
    )
endif()

##############################################################################
# Sanitizer Configuration
##############################################################################

if(HFT_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        set(HFT_SANITIZER_FLAGS
            -fsanitize=address
            -fsanitize=undefined
            -fno-sanitize-recover=all
        )
    endif()
endif()

##############################################################################
# Link-Time Optimization
##############################################################################

if(HFT_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported)
    if(lto_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

##############################################################################
# Core Header-Only Library
##############################################################################

add_library(hft_nanotick INTERFACE)
add_library(HFT::NanoTick ALIAS hft_nanotick)

target_include_directories(hft_nanotick
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(hft_nanotick INTERFACE cxx_std_20)

# Platform-specific threading
find_package(Threads REQUIRED)
target_link_libraries(hft_nanotick INTERFACE Threads::Threads)

##############################################################################
# Dependencies
##############################################################################

# The core library is protocol-neutral. QuickFIX-specific code is guarded by
# HFT_ENABLE_QUICKFIX and should be wired through a separate adapter build.

# Apply compiler flags to interface
target_compile_options(hft_nanotick INTERFACE
    $<$<CONFIG:Release>:${HFT_OPTIMIZATION_FLAGS}>
    ${HFT_WARNING_FLAGS}
)

if(HFT_ENABLE_SANITIZERS)
    target_compile_options(hft_nanotick INTERFACE ${HFT_SANITIZER_FLAGS})
    target_link_options(hft_nanotick INTERFACE ${HFT_SANITIZER_FLAGS})
endif()

##############################################################################
# Example Application
##############################################################################

add_executable(hft_example src/main.cpp)
target_link_libraries(hft_example PRIVATE HFT::NanoTick)

target_compile_options(hft_example PRIVATE
    $<$<CONFIG:Release>:${HFT_OPTIMIZATION_FLAGS}>
    ${HFT_WARNING_FLAGS}
)

##############################################################################
# Testing
##############################################################################

if(HFT_BUILD_TESTS)
    enable_testing()

    # Fetch GoogleTest
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    # Test executable
    add_executable(hft_tests
        tests/test_gateway.cpp
        tests/test_latency.cpp
        tests/test_orderbook.cpp
        tests/test_public_headers.cpp
        tests/test_spsc_queue.cpp
        tests/test_strategy.cpp
        # tests/test_fix_parser.cpp  # Disabled - requires QuickFIX
    )

    target_link_libraries(hft_tests
        PRIVATE
            HFT::NanoTick
            GTest::gtest
            GTest::gtest_main
    )

    target_compile_options(hft_tests PRIVATE
        ${HFT_WARNING_FLAGS}
    )

    include(GoogleTest)
    gtest_discover_tests(hft_tests)
endif()

##############################################################################
# Benchmarks
##############################################################################

if(HFT_BUILD_BENCHMARKS)
    include(FetchContent)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(hft_benchmarks
        benchmarks/bench_orderbook.cpp
        benchmarks/bench_spsc_queue.cpp
        # benchmarks/bench_latency.cpp  # Disabled - requires QuickFIX
    )

    target_link_libraries(hft_benchmarks
        PRIVATE
            HFT::NanoTick
            benchmark::benchmark
            benchmark::benchmark_main
    )

    target_compile_options(hft_benchmarks PRIVATE
        $<$<CONFIG:Release>:${HFT_OPTIMIZATION_FLAGS}>
        ${HFT_WARNING_FLAGS}
    )
endif()

##############################################################################
# Installation (disabled - standalone project)
##############################################################################

# Installation is disabled for this standalone prototype. If packaging is
# needed, add install/export rules for the header-only target and keep protocol
# adapters in separately discovered optional packages.

##############################################################################
# Summary
##############################################################################

message(STATUS "")
message(STATUS "HFT NanoTick Configuration Summary")
message(STATUS "===================================")
message(STATUS "  Version:          ${PROJECT_VERSION}")
message(STATUS "  C++ Standard:     C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:         ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build Tests:      ${HFT_BUILD_TESTS}")
message(STATUS "  Build Benchmarks: ${HFT_BUILD_BENCHMARKS}")
message(STATUS "  Sanitizers:       ${HFT_ENABLE_SANITIZERS}")
message(STATUS "  LTO:              ${HFT_ENABLE_LTO}")
message(STATUS "  Native Arch:      ${HFT_NATIVE_ARCH}")
message(STATUS "")
