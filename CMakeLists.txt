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
    "FFT backend: auto|vdsp|cmsis|mkl|aocl|armpl|pocketfft|portable")
set_property(CACHE FFT_BACKEND PROPERTY STRINGS
    auto vdsp cmsis mkl aocl armpl pocketfft portable)

set(FIR_BACKEND "portable" CACHE STRING
    "FIR backend: portable|liquid|ipp|cmsis")
set_property(CACHE FIR_BACKEND PROPERTY STRINGS
    portable liquid ipp cmsis)

option(FFT_ENABLE_BENCHMARKS "Build the benchmark harness" OFF)
option(FFT_ENABLE_KFR_BENCHMARK "Build the benchmark-only KFR comparison" OFF)
option(FFT_ENABLE_MKL_BENCHMARK "Build the Intel oneMKL benchmark backend (fetched)" OFF)
option(FFT_ENABLE_AOCL_BENCHMARK "Build the AMD AOCL-FFTZ benchmark backend (fetched + built)" OFF)
option(FFT_ENABLE_ARMPL_BENCHMARK "Build the Arm Performance Libraries benchmark backend (fetched)" OFF)
option(FFT_ENABLE_POCKETFFT_BENCHMARK "Build the PocketFFT benchmark backend (fetched header)" OFF)
option(FFT_ENABLE_CMSIS_BENCHMARK "Build the Arm CMSIS-DSP FFT benchmark backend (fetched)" OFF)
option(FFT_ENABLE_FFTW_BENCHMARK "Build the FFTW FFT benchmark backend (fetched; benchmark-only, GPL)" OFF)
option(FIR_ENABLE_LIQUID_BENCHMARK "Build the liquid-dsp FIR benchmark backend (fetched)" OFF)
option(FIR_ENABLE_KFR_BENCHMARK "Build the benchmark-only KFR FIR comparison" OFF)
option(FIR_ENABLE_IPP_BENCHMARK "Build the Intel IPP FIR benchmark backend (fetched with uv)" OFF)
option(FIR_ENABLE_CMSIS_BENCHMARK "Build the Arm CMSIS-DSP FIR benchmark backend (fetched)" OFF)
option(FFT_ENABLE_ALL_BENCHMARKS "Build every FFT benchmark backend compatible with this host" OFF)
# DSP-wide umbrella: like FFT_ENABLE_ALL_BENCHMARKS but also covers the FIR-only
# backends (adds liquid-dsp). This is the switch the Makefile flips so it never
# has to name individual backends -- CMake host-gates each one below.
option(DSP_ENABLE_ALL_BENCHMARKS "Build every FFT and FIR benchmark backend compatible with this host" OFF)
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

# ---------------------------------------------------------------------------
# Testable library-backend manifest (single source of truth for CI)
# ---------------------------------------------------------------------------
# The correctness test matrix in .github/workflows/ is GENERATED from this table
# rather than hardcoded in YAML: CI configures once, reads the emitted
# testable_backends.json, and fans a job out per backend for each arch. Adding or
# removing a library backend here is therefore the only edit CI needs -- the same
# "choosing backends is CMake's job" principle the Makefile follows for benchmarks.
#
# The table is deliberately STATIC (host-independent) so one configure on any
# machine yields both the x86 and Arm lists. Compatibility rules:
#   - portable / cmsis / liquid : portable C, buildable and tested on every arch.
#   - pocketfft                 : portable C++ (BSD-3-Clause), every arch.
#   - mkl / aocl / ipp          : x86-only vendor libraries.
#   - armpl                     : AArch64-only vendor library (fetched by
#                                 cmake/FetchArmPL.cmake), so Arm-tested only.
#   - vdsp                      : Apple-only AND still a compile-error stub, so it
#                                 is intentionally omitted from the tested set.
set(_fft_backends_x86 portable cmsis pocketfft mkl aocl)
set(_fft_backends_arm portable cmsis pocketfft armpl)
set(_fir_backends_x86 portable cmsis liquid ipp)
set(_fir_backends_arm portable cmsis liquid)

# Render a CMake list as a compact JSON array of strings: a b c -> ["a","b","c"].
function(_dsp_json_string_array _out)
    set(_quoted "")
    foreach(_item ${ARGN})
        list(APPEND _quoted "\"${_item}\"")
    endforeach()
    list(JOIN _quoted "," _joined)
    set(${_out} "[${_joined}]" PARENT_SCOPE)
endfunction()

_dsp_json_string_array(_json_fft_x86 ${_fft_backends_x86})
_dsp_json_string_array(_json_fft_arm ${_fft_backends_arm})
_dsp_json_string_array(_json_fir_x86 ${_fir_backends_x86})
_dsp_json_string_array(_json_fir_arm ${_fir_backends_arm})

file(WRITE "${CMAKE_BINARY_DIR}/testable_backends.json"
"{
  \"fft\": { \"x86\": ${_json_fft_x86}, \"arm\": ${_json_fft_arm} },
  \"fir\": { \"x86\": ${_json_fir_x86}, \"arm\": ${_json_fir_arm} }
}
")

# Which optional benchmark backends can this host build? (portable is always
# available and needs no external dependency.) Best-effort guide:
#   - KFR:        x86 (SSE2+, any compiler); Arm (NEON) requires Clang, since
#                 KFR's NEON intrinsics don't compile with GCC.
#   - oneMKL:     Intel x86 library, x86 only.
#   - AOCL-FFTZ:  AMD x86 library, x86 only.
#   - IPP:        Intel x86 library, x86 only.
#   - ArmPL:      Arm AArch64 vendor library, Arm only (fetched deb, no root).
#   - liquid-dsp: portable C, every host.
#   - FFTW:       portable C (SSE2/AVX/AVX2 on x86, NEON on Arm), every host.
#   - PocketFFT:  portable C++ (BSD-3-Clause header), every host.
set(_compat_kfr FALSE)
set(_compat_mkl FALSE)
set(_compat_aocl FALSE)
set(_compat_ipp FALSE)
set(_compat_armpl FALSE)
# PocketFFT is a portable C++ header, compatible with every host we target.
set(_compat_pocketfft TRUE)
# CMSIS-DSP, liquid-dsp and FFTW are portable C (scalar/SIMD host build on x86,
# NEON on Arm), so they are compatible with every host we target.
set(_compat_cmsis TRUE)
set(_compat_liquid TRUE)
set(_compat_fftw TRUE)
if(_host_x86)
    set(_compat_kfr TRUE)
    set(_compat_mkl TRUE)
    set(_compat_aocl TRUE)
    set(_compat_ipp TRUE)
elseif(_host_arm)
    # ArmPL is the AArch64 vendor library (the Arm counterpart to x86 oneMKL/AOCL),
    # fetched by cmake/FetchArmPL.cmake. The GCC (gfortran-ABI) build links cleanly
    # with both GCC and vanilla Clang, so it is compatible on every Arm host.
    set(_compat_armpl TRUE)
    # KFR's hand-written ARM NEON kernels (its KFR_NATIVE_INTRINSICS path) only
    # compile with Clang; GCC rejects KFR's signed vbslq_* select intrinsics. So
    # KFR is a compatible benchmark backend on Arm only under a Clang toolchain
    # -- otherwise FFT_ENABLE_ALL_BENCHMARKS would auto-select a backend that
    # cannot build here. The FATAL_ERROR guard below still catches an explicit
    # -DFFT_ENABLE_KFR_BENCHMARK=ON on Arm+GCC with an actionable message.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_compat_kfr TRUE)
    endif()
endif()

# FFT_ENABLE_ALL_BENCHMARKS / DSP_ENABLE_ALL_BENCHMARKS: convenience switches
# equivalent to setting the individual *_ENABLE_*_BENCHMARK flags for every
# backend compatible with this host (and the harness itself). The DSP-wide
# switch additionally enables the FIR-only backends (liquid-dsp). Non-cache
# sets, so they never claim an incompatible backend (e.g. x86-only IPP on Arm)
# and never rewrite the user's cached values.
if(FFT_ENABLE_ALL_BENCHMARKS OR DSP_ENABLE_ALL_BENCHMARKS)
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
    if(_compat_fftw)
        set(FFT_ENABLE_FFTW_BENCHMARK ON)
    endif()
    if(_compat_mkl)
        set(FFT_ENABLE_MKL_BENCHMARK ON)
    endif()
    if(_compat_aocl)
        set(FFT_ENABLE_AOCL_BENCHMARK ON)
    endif()
    if(_compat_armpl)
        set(FFT_ENABLE_ARMPL_BENCHMARK ON)
    endif()
    if(_compat_pocketfft)
        set(FFT_ENABLE_POCKETFFT_BENCHMARK ON)
    endif()
endif()
# liquid-dsp is a FIR-only backend and was never part of the FFT-era switch, so
# only the DSP-wide umbrella enables it (keeps FFT_ENABLE_ALL_BENCHMARKS's
# behavior unchanged for anyone relying on it).
if(DSP_ENABLE_ALL_BENCHMARKS AND _compat_liquid)
    set(FIR_ENABLE_LIQUID_BENCHMARK ON)
endif()

# ---------------------------------------------------------------------------
# Backend resolution
# ---------------------------------------------------------------------------
set(_resolved_backend "${FFT_BACKEND}")
if(_resolved_backend STREQUAL "auto")
    if(APPLE)
        set(_resolved_backend "vdsp")
    elseif(_host_arm AND NOT WIN32)
        # ArmPL is a complete AArch64 vendor backend (fetched on demand via
        # cmake/FetchArmPL.cmake), the Arm counterpart to AOCL on x86, so it is
        # the default on Arm. (Set -DFFT_BACKEND=cmsis for the dependency-free,
        # download-free portable/NEON path.)
        set(_resolved_backend "armpl")
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
if(_compat_armpl)
    list(APPEND _compat_list "armpl")
endif()
if(_compat_pocketfft)
    list(APPEND _compat_list "pocketfft")
endif()
if(_compat_ipp)
    list(APPEND _compat_list "ipp")
endif()
if(_compat_cmsis)
    list(APPEND _compat_list "cmsis")
endif()
if(_compat_liquid)
    list(APPEND _compat_list "liquid")
endif()
if(_compat_fftw)
    list(APPEND _compat_list "fftw")
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
elseif(_resolved_backend STREQUAL "armpl")
    list(APPEND _backend_sources src/backends/armpl/armpl_backend.cpp)
elseif(_resolved_backend STREQUAL "pocketfft")
    list(APPEND _backend_sources src/backends/pocketfft/pocketfft_backend.cpp)
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

# The ArmPL backend needs the fetched Arm Performance Libraries (its FFTW3
# interface header and the LP64 static library).
if(_resolved_backend STREQUAL "armpl")
    include(FetchArmPL)
    target_link_libraries(dsp_dispatch PRIVATE ArmPL::ArmPL)
endif()

# The PocketFFT backend needs the fetched header-only PocketFFT (portable C++).
if(_resolved_backend STREQUAL "pocketfft")
    include(FetchPocketFFT)
    target_link_libraries(dsp_dispatch PRIVATE PocketFFT::PocketFFT)
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
# KFR's hand-written ARM NEON kernels (its KFR_NATIVE_INTRINSICS path) only
# compile with Clang. GCC rejects KFR's signed vbslq_* select intrinsics (the
# mask arg is e.g. int16x8_t, but GCC's vbslq_s16 wants uint16x8_t), so an
# aarch64 GCC build fails ~200 lines deep inside KFR. Fail fast here with an
# actionable message instead. x86 is unaffected (that path uses SSE/AVX, not
# these NEON intrinsics), so this is gated on _host_arm only.
if(_host_arm AND (FFT_ENABLE_KFR_BENCHMARK OR FIR_ENABLE_KFR_BENCHMARK)
        AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "KFR benchmarks on Arm require a Clang toolchain; "
        "${CMAKE_CXX_COMPILER_ID} cannot compile KFR's NEON intrinsics. "
        "Re-run CMake with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++, "
        "or drop the KFR benchmark (omit FFT_ENABLE_ALL_BENCHMARKS, or set "
        "-DFFT_ENABLE_KFR_BENCHMARK=OFF -DFIR_ENABLE_KFR_BENCHMARK=OFF).")
endif()

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

# FFTW is GPL and benchmark-only: same rules as KFR. It may be benchmarked in an
# optimized Release build, but must never enter a packaged/redistributable
# artifact. Enforced here and by CI's dependency/packaged-file inspection.
if(FFT_ENABLE_FFTW_BENCHMARK)
    if(NOT FFT_ENABLE_BENCHMARKS)
        message(FATAL_ERROR "FFT_ENABLE_FFTW_BENCHMARK requires FFT_ENABLE_BENCHMARKS=ON.")
    endif()
    if(FFT_PACKAGING)
        message(FATAL_ERROR
            "FFT_ENABLE_FFTW_BENCHMARK is ON while FFT_PACKAGING is ON. FFTW is "
            "GPL and benchmark-only; it must never be shipped. Disable one of them.")
    endif()
    message(WARNING
        "FFTW benchmark enabled: internal performance comparison only. The FFTW "
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

if(FFT_ENABLE_ARMPL_BENCHMARK AND NOT FFT_ENABLE_BENCHMARKS)
    message(FATAL_ERROR "FFT_ENABLE_ARMPL_BENCHMARK requires FFT_ENABLE_BENCHMARKS=ON.")
endif()

if(FFT_ENABLE_POCKETFFT_BENCHMARK AND NOT FFT_ENABLE_BENCHMARKS)
    message(FATAL_ERROR "FFT_ENABLE_POCKETFFT_BENCHMARK requires FFT_ENABLE_BENCHMARKS=ON.")
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
