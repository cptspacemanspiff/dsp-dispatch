cmake_minimum_required(VERSION 3.20)
project(fft_dispatch LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options (Phase 4: build and licensing controls)
# ---------------------------------------------------------------------------
set(FFT_BACKEND "auto" CACHE STRING
    "FFT backend: auto|vdsp|cmsis|mkl|aocl|portable")
set_property(CACHE FFT_BACKEND PROPERTY STRINGS
    auto vdsp cmsis mkl aocl portable)

option(FFT_ENABLE_BENCHMARKS "Build the benchmark harness" OFF)
option(FFT_ENABLE_KFR_BENCHMARK "Build the benchmark-only KFR comparison" OFF)
option(FFT_ENABLE_MKL_BENCHMARK "Build the Intel oneMKL benchmark backend (fetched)" OFF)
option(FFT_ENABLE_AOCL_BENCHMARK "Build the AMD AOCL-FFTZ benchmark backend (fetched + built)" OFF)
option(FFT_BUILD_TESTS "Build correctness tests" ON)
# Set ON in packaging/release CI to assert no forbidden libraries are present.
# Benchmarking KFR in an optimized (Release) build is allowed; packaging it is not.
option(FFT_PACKAGING "Building a shippable/redistributable artifact" OFF)

# ---------------------------------------------------------------------------
# Backend resolution
# ---------------------------------------------------------------------------
set(_resolved_backend "${FFT_BACKEND}")
if(_resolved_backend STREQUAL "auto")
    if(APPLE)
        set(_resolved_backend "vdsp")
    elseif((CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64") AND NOT WIN32)
        set(_resolved_backend "cmsis")
    else()
        # Only the portable backend is implemented today; vendor backends are
        # scaffolded and emit a clear error if selected. Auto stays portable so
        # every platform has a correct, buildable default.
        set(_resolved_backend "portable")
    endif()
endif()
message(STATUS "fft-dispatch backend: ${_resolved_backend}")

set(_backend_sources "")
if(_resolved_backend STREQUAL "portable")
    list(APPEND _backend_sources src/backends/portable/portable_backend.cpp)
elseif(_resolved_backend STREQUAL "vdsp")
    list(APPEND _backend_sources src/backends/vdsp/vdsp_backend.cpp)
elseif(_resolved_backend STREQUAL "cmsis")
    list(APPEND _backend_sources src/backends/cmsis/cmsis_backend.cpp)
elseif(_resolved_backend STREQUAL "mkl")
    list(APPEND _backend_sources src/backends/mkl/mkl_backend.cpp)
elseif(_resolved_backend STREQUAL "aocl")
    list(APPEND _backend_sources src/backends/aocl/aocl_backend.cpp)
else()
    message(FATAL_ERROR "Unknown FFT_BACKEND: ${FFT_BACKEND}")
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(fft_dispatch
    src/common/fft.cpp
    ${_backend_sources})
target_include_directories(fft_dispatch
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(fft_dispatch PUBLIC cxx_std_17)
target_compile_definitions(fft_dispatch PRIVATE
    FFT_BACKEND_NAME="${_resolved_backend}")

# The MKL backend needs the fetched oneMKL headers and static libraries.
if(_resolved_backend STREQUAL "mkl")
    include(FetchMKL)
    target_link_libraries(fft_dispatch PRIVATE MKL::MKL)
endif()

# The AOCL backend needs AOCL-FFTZ built from source and its headers.
if(_resolved_backend STREQUAL "aocl")
    include(FetchAOCL)
    target_link_libraries(fft_dispatch PRIVATE AOCL::FFTZ)
endif()

# Only the library and public headers are installable/redistributable. The
# benchmark target (the sole carrier of KFR) has no install rule, so it can
# never be packaged.
install(TARGETS fft_dispatch ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

# ---------------------------------------------------------------------------
# Licensing safeguards (Phase 4)
# ---------------------------------------------------------------------------
# KFR is benchmark-only and must never enter a packaged/redistributable artifact.
# It may be benchmarked in an optimized Release build (that is the point of the
# comparison); the prohibition is on PACKAGING it, enforced here and by CI's
# dependency/packaged-file inspection.
if(FFT_ENABLE_KFR_BENCHMARK)
    if(NOT FFT_ENABLE_BENCHMARKS)
        message(FATAL_ERROR "FFT_ENABLE_KFR_BENCHMARK requires FFT_ENABLE_BENCHMARKS=ON.")
    endif()
    if(FFT_PACKAGING)
        message(FATAL_ERROR
            "FFT_ENABLE_KFR_BENCHMARK is ON while FFT_PACKAGING is ON. KFR is "
            "benchmark-only and must never be shipped. Disable one of them.")
    endif()
    message(WARNING
        "KFR benchmark enabled: internal performance comparison only. The KFR "
        "code stays under bench/ and is never linked into the library or installed.")
endif()

# ---------------------------------------------------------------------------
# Tests and benchmarks
# ---------------------------------------------------------------------------
if(FFT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FFT_ENABLE_BENCHMARKS)
    # Prefer a system-installed Google Benchmark; otherwise fetch a pinned tag.
    find_package(benchmark QUIET)
    if(NOT benchmark_FOUND)
        message(STATUS "Google Benchmark not found; fetching via FetchContent.")
        include(FetchContent)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(benchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG v1.9.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(benchmark)
    endif()
    add_subdirectory(bench)
endif()
