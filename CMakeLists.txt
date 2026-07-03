cmake_minimum_required(VERSION 3.20)
project(dsp_dispatch LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Host-native tuning
# ---------------------------------------------------------------------------
# -march=native lets the compiler use the full ISA of the build machine
# (AVX2/FMA, AVX-512, ...) for code we compile from source. Without it, GCC/Clang
# target the generic x86-64 baseline (SSE2 only), so -O3 auto-vectorization is
# capped at 128-bit SSE2 and never emits AVX2/FMA even on capable hardware.
#
# Applied only to OUR targets (the library and the benchmark wrappers/executables)
# via DSP_ARCH_FLAGS. Fetched third-party deps are left untouched: liquid-dsp runs
# its own SIMD detection, and KFR manages per-arch flags internally (a blanket
# -march=native would fight its architecture machinery). Override with
# -DDSP_NATIVE_ARCH=OFF for reproducible or cross-compiled builds.
option(DSP_NATIVE_ARCH "Compile dsp-dispatch's own code with -march=native" ON)
set(DSP_ARCH_FLAGS "")
if(DSP_NATIVE_ARCH)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" _dsp_has_march_native)
    if(_dsp_has_march_native)
        set(DSP_ARCH_FLAGS -march=native)
        message(STATUS "dsp-dispatch: building own code with -march=native")
    else()
        message(STATUS "dsp-dispatch: -march=native unsupported by this compiler; skipping")
    endif()
endif()

# ---------------------------------------------------------------------------
# Options (Phase 4: build and licensing controls)
# ---------------------------------------------------------------------------
set(FFT_BACKEND "auto" CACHE STRING
    "FFT backend: auto|vdsp|cmsis|mkl|aocl|portable")
set_property(CACHE FFT_BACKEND PROPERTY STRINGS
    auto vdsp cmsis mkl aocl portable)

set(FIR_BACKEND "portable" CACHE STRING
    "FIR backend: portable|liquid|ipp|cmsis")
set_property(CACHE FIR_BACKEND PROPERTY STRINGS
    portable liquid ipp cmsis)

option(FFT_ENABLE_BENCHMARKS "Build the benchmark harness" OFF)
option(FFT_ENABLE_KFR_BENCHMARK "Build the benchmark-only KFR comparison" OFF)
option(FFT_ENABLE_MKL_BENCHMARK "Build the Intel oneMKL benchmark backend (fetched)" OFF)
option(FFT_ENABLE_AOCL_BENCHMARK "Build the AMD AOCL-FFTZ benchmark backend (fetched + built)" OFF)
option(FFT_ENABLE_CMSIS_BENCHMARK "Build the Arm CMSIS-DSP FFT benchmark backend (fetched)" OFF)
option(FIR_ENABLE_LIQUID_BENCHMARK "Build the liquid-dsp FIR benchmark backend (fetched)" OFF)
option(FIR_ENABLE_KFR_BENCHMARK "Build the benchmark-only KFR FIR comparison" OFF)
option(FIR_ENABLE_IPP_BENCHMARK "Build the Intel IPP FIR benchmark backend (fetched with uv)" OFF)
option(FIR_ENABLE_CMSIS_BENCHMARK "Build the Arm CMSIS-DSP FIR benchmark backend (fetched)" OFF)
option(FFT_ENABLE_ALL_BENCHMARKS "Build every benchmark backend compatible with this host" OFF)
option(FFT_BUILD_TESTS "Build correctness tests" ON)
# Set ON in packaging/release CI to assert no forbidden libraries are present.
# Benchmarking KFR in an optimized (Release) build is allowed; packaging it is not.
option(FFT_PACKAGING "Building a shippable/redistributable artifact" OFF)

# ---------------------------------------------------------------------------
# Host detection and backend compatibility
# ---------------------------------------------------------------------------
# Classify the target CPU once; reused by both the library backend selection
# and the "all compatible benchmarks" expansion below.
set(_host_x86 FALSE)
set(_host_arm FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86|x86")
    set(_host_x86 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
    set(_host_arm TRUE)
endif()

# Which optional benchmark backends can this host build? (portable is always
# available and needs no external dependency.) Best-effort guide:
#   - KFR:        cross-platform (x86 SSE2+ / ARM NEON).
#   - oneMKL:     Intel x86 library, x86 only.
#   - AOCL-FFTZ:  AMD x86 library, x86 only.
#   - IPP:        Intel x86 library, x86 only.
set(_compat_kfr FALSE)
set(_compat_mkl FALSE)
set(_compat_aocl FALSE)
set(_compat_ipp FALSE)
# CMSIS-DSP is portable C (scalar host build on x86, NEON on Arm), so it is
# compatible with every host we target.
set(_compat_cmsis TRUE)
if(_host_x86)
    set(_compat_kfr TRUE)
    set(_compat_mkl TRUE)
    set(_compat_aocl TRUE)
    set(_compat_ipp TRUE)
elseif(_host_arm)
    set(_compat_kfr TRUE)
endif()

# FFT_ENABLE_ALL_BENCHMARKS: convenience switch equivalent to setting the
# individual FFT_ENABLE_*_BENCHMARK flags for every backend compatible with this
# host (and the harness itself). Non-cache sets, so it never claims an
# incompatible backend and never rewrites the user's cached values.
if(FFT_ENABLE_ALL_BENCHMARKS)
    set(FFT_ENABLE_BENCHMARKS ON)
    if(_compat_kfr)
        set(FFT_ENABLE_KFR_BENCHMARK ON)
        set(FIR_ENABLE_KFR_BENCHMARK ON)
    endif()
    if(_compat_ipp)
        set(FIR_ENABLE_IPP_BENCHMARK ON)
    endif()
    if(_compat_cmsis)
        set(FIR_ENABLE_CMSIS_BENCHMARK ON)
        set(FFT_ENABLE_CMSIS_BENCHMARK ON)
    endif()
    if(_compat_mkl)
        set(FFT_ENABLE_MKL_BENCHMARK ON)
    endif()
    if(_compat_aocl)
        set(FFT_ENABLE_AOCL_BENCHMARK ON)
    endif()
endif()

# ---------------------------------------------------------------------------
# Backend resolution
# ---------------------------------------------------------------------------
set(_resolved_backend "${FFT_BACKEND}")
if(_resolved_backend STREQUAL "auto")
    if(APPLE)
        set(_resolved_backend "vdsp")
    elseif(_host_arm AND NOT WIN32)
        set(_resolved_backend "cmsis")
    elseif(_host_x86)
        # AOCL-FFTZ is a complete x86 backend (fetched and built from source via
        # cmake/FetchAOCL.cmake), so it is the default on x86.
        set(_resolved_backend "aocl")
    else()
        # Fallback for architectures without a vendor backend. The portable
        # backend is always implemented, so every platform stays buildable.
        set(_resolved_backend "portable")
    endif()
endif()

# Configure-time build audit: selected library backend plus the optional
# benchmark backends that are compatible with this host.
set(_compat_list "portable")
if(_compat_kfr)
    list(APPEND _compat_list "kfr")
endif()
if(_compat_mkl)
    list(APPEND _compat_list "mkl")
endif()
if(_compat_aocl)
    list(APPEND _compat_list "aocl")
endif()
if(_compat_ipp)
    list(APPEND _compat_list "ipp")
endif()
if(_compat_cmsis)
    list(APPEND _compat_list "cmsis")
endif()
string(REPLACE ";" " " _compat_str "${_compat_list}")
message(STATUS "dsp-dispatch backend: ${_resolved_backend}")
message(STATUS "dsp-dispatch FIR backend: ${FIR_BACKEND}")
message(STATUS "dsp-dispatch host: ${CMAKE_SYSTEM_PROCESSOR} | "
    "compatible benchmark backends: ${_compat_str}")

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

set(_fir_backend_sources "")
if(FIR_BACKEND STREQUAL "portable")
    list(APPEND _fir_backend_sources src/backends/portable/fir_backend.cpp)
elseif(FIR_BACKEND STREQUAL "liquid")
    list(APPEND _fir_backend_sources src/backends/liquid/liquid_fir_backend.cpp)
elseif(FIR_BACKEND STREQUAL "ipp")
    list(APPEND _fir_backend_sources src/backends/ipp/ipp_fir_backend.cpp)
elseif(FIR_BACKEND STREQUAL "cmsis")
    list(APPEND _fir_backend_sources src/backends/cmsis/cmsis_fir_backend.cpp)
else()
    message(FATAL_ERROR "Unknown FIR_BACKEND: ${FIR_BACKEND}")
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(dsp_dispatch
    src/common/fft.cpp
    src/common/fir.cpp
    ${_backend_sources}
    ${_fir_backend_sources})
target_include_directories(dsp_dispatch
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(dsp_dispatch PUBLIC cxx_std_17)
target_compile_options(dsp_dispatch PRIVATE ${DSP_ARCH_FLAGS})
target_compile_definitions(dsp_dispatch PRIVATE
    FFT_BACKEND_NAME="${_resolved_backend}")

# The MKL backend needs the fetched oneMKL headers and static libraries.
if(_resolved_backend STREQUAL "mkl")
    include(FetchMKL)
    target_link_libraries(dsp_dispatch PRIVATE MKL::MKL)
endif()

# The AOCL backend needs AOCL-FFTZ built from source and its headers.
if(_resolved_backend STREQUAL "aocl")
    include(FetchAOCL)
    target_link_libraries(dsp_dispatch PRIVATE AOCL::FFTZ)
endif()

# The CMSIS FFT backend needs CMSIS-DSP (scalar on x86, NEON on Arm).
if(_resolved_backend STREQUAL "cmsis")
    include(FetchCMSISDSP)
    target_link_libraries(dsp_dispatch PRIVATE CMSISDSP::CMSISDSP)
endif()

if(FIR_BACKEND STREQUAL "liquid")
    include(FetchLiquidDSP)
    target_link_libraries(dsp_dispatch PRIVATE LiquidDSP::LiquidDSP)
endif()

if(FIR_BACKEND STREQUAL "ipp")
    include(FetchIPP)
    target_link_libraries(dsp_dispatch PRIVATE IPP::IPP)
endif()

if(FIR_BACKEND STREQUAL "cmsis")
    include(FetchCMSISDSP)
    target_link_libraries(dsp_dispatch PRIVATE CMSISDSP::CMSISDSP)
endif()

# Only the library and public headers are installable/redistributable. The
# benchmark target (the sole carrier of KFR) has no install rule, so it can
# never be packaged.
install(TARGETS dsp_dispatch ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
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

if(FIR_ENABLE_KFR_BENCHMARK)
    if(NOT FFT_ENABLE_BENCHMARKS)
        message(FATAL_ERROR "FIR_ENABLE_KFR_BENCHMARK requires FFT_ENABLE_BENCHMARKS=ON.")
    endif()
    if(FFT_PACKAGING)
        message(FATAL_ERROR
            "FIR_ENABLE_KFR_BENCHMARK is ON while FFT_PACKAGING is ON. KFR is "
            "benchmark-only and must never be shipped. Disable one of them.")
    endif()
    message(WARNING
        "KFR FIR benchmark enabled: internal performance comparison only. The KFR "
        "code stays under bench/ and is never linked into the library or installed.")
endif()

if(FIR_ENABLE_LIQUID_BENCHMARK AND NOT FFT_ENABLE_BENCHMARKS)
    message(FATAL_ERROR "FIR_ENABLE_LIQUID_BENCHMARK requires FFT_ENABLE_BENCHMARKS=ON.")
endif()

if(FIR_ENABLE_IPP_BENCHMARK AND NOT FFT_ENABLE_BENCHMARKS)
    message(FATAL_ERROR "FIR_ENABLE_IPP_BENCHMARK requires FFT_ENABLE_BENCHMARKS=ON.")
endif()

if(FIR_ENABLE_CMSIS_BENCHMARK AND NOT FFT_ENABLE_BENCHMARKS)
    message(FATAL_ERROR "FIR_ENABLE_CMSIS_BENCHMARK requires FFT_ENABLE_BENCHMARKS=ON.")
endif()

if(FFT_ENABLE_CMSIS_BENCHMARK AND NOT FFT_ENABLE_BENCHMARKS)
    message(FATAL_ERROR "FFT_ENABLE_CMSIS_BENCHMARK requires FFT_ENABLE_BENCHMARKS=ON.")
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
