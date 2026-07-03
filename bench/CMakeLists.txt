# Per-backend benchmark executables.
#
# Each benchmarked backend becomes its own static library (shared common code +
# that backend's BackendPlan implementation) and its own executable built from
# the single backend-agnostic harness (bench_fft.cpp). This is what lets you run
# `fft_bench_pocketfft` and `fft_bench_kfr` side by side from one build.

# fft_add_bench_backend(<name> <backend_source>)
# Extra args after the source are passed to target_link_libraries on the lib.
function(fft_add_bench_backend name backend_src)
    add_library(dsp_dispatch_${name} STATIC
        ${PROJECT_SOURCE_DIR}/src/common/fft.cpp
        ${backend_src})
    target_include_directories(dsp_dispatch_${name}
        PUBLIC ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(dsp_dispatch_${name} PUBLIC cxx_std_17)
    target_compile_options(dsp_dispatch_${name} PRIVATE ${DSP_ARCH_FLAGS})
    if(ARGN)
        target_link_libraries(dsp_dispatch_${name} PUBLIC ${ARGN})
    endif()

    add_executable(fft_bench_${name} ${CMAKE_CURRENT_SOURCE_DIR}/bench_fft.cpp)
    target_link_libraries(fft_bench_${name}
        PRIVATE dsp_dispatch_${name} benchmark::benchmark)
    target_compile_options(fft_bench_${name} PRIVATE ${DSP_ARCH_FLAGS})
    target_include_directories(fft_bench_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Record it so the `fft_benchmarks` aggregate target below builds exactly the
    # backends CMake actually configured -- no hardcoded list in the Makefile.
    set_property(GLOBAL APPEND PROPERTY DSP_FFT_BENCH_TARGETS fft_bench_${name})
endfunction()

# PocketFFT is the always-available FFT baseline (registered below, auto-enabled
# whenever the benchmark harness is built); it is fetched header-only.

# fir_add_bench_backend(<name> <backend_source>)
# Extra args after the source are passed to target_link_libraries on the lib.
function(fir_add_bench_backend name backend_src)
    add_library(dsp_dispatch_fir_${name} STATIC
        ${PROJECT_SOURCE_DIR}/src/common/fir.cpp
        ${backend_src})
    target_include_directories(dsp_dispatch_fir_${name}
        PUBLIC ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(dsp_dispatch_fir_${name} PUBLIC cxx_std_17)
    target_compile_options(dsp_dispatch_fir_${name} PRIVATE ${DSP_ARCH_FLAGS})
    if(ARGN)
        target_link_libraries(dsp_dispatch_fir_${name} PUBLIC ${ARGN})
    endif()

    add_executable(fir_bench_${name} ${CMAKE_CURRENT_SOURCE_DIR}/bench_fir.cpp)
    target_link_libraries(fir_bench_${name}
        PRIVATE dsp_dispatch_fir_${name} benchmark::benchmark)
    target_compile_options(fir_bench_${name} PRIVATE ${DSP_ARCH_FLAGS})
    target_include_directories(fir_bench_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Record it so the `fir_benchmarks` aggregate target below builds exactly the
    # backends CMake actually configured -- no hardcoded list in the Makefile.
    set_property(GLOBAL APPEND PROPERTY DSP_FIR_BENCH_TARGETS fir_bench_${name})
endfunction()

# FIR portable: always available, no external dependencies.
fir_add_bench_backend(portable
    ${PROJECT_SOURCE_DIR}/src/backends/portable/fir_backend.cpp)

if(FIR_ENABLE_LIQUID_BENCHMARK)
    include(FetchLiquidDSP)
    fir_add_bench_backend(liquid
        ${PROJECT_SOURCE_DIR}/src/backends/liquid/liquid_fir_backend.cpp
        LiquidDSP::LiquidDSP)
    message(STATUS "liquid-dsp FIR benchmark backend enabled (fetched).")
endif()

if(FIR_ENABLE_IPP_BENCHMARK)
    include(FetchIPP)
    fir_add_bench_backend(ipp
        ${PROJECT_SOURCE_DIR}/src/backends/ipp/ipp_fir_backend.cpp
        IPP::IPP)
    message(STATUS "IPP FIR benchmark backend enabled (static ipp-static via uv if needed).")
endif()

if(FIR_ENABLE_CMSIS_BENCHMARK)
    include(FetchCMSISDSP)
    fir_add_bench_backend(cmsis
        ${PROJECT_SOURCE_DIR}/src/backends/cmsis/cmsis_fir_backend.cpp
        CMSISDSP::CMSISDSP)
    message(STATUS "CMSIS-DSP FIR benchmark backend enabled (fetched; portable/host, compiler auto-vectorizes to NEON on Arm).")
endif()

# KFR: benchmark-only, fetched on demand. Lives entirely under bench/.
if(FFT_ENABLE_KFR_BENCHMARK OR FIR_ENABLE_KFR_BENCHMARK)
    include(FetchContent)
    if(FFT_ENABLE_KFR_BENCHMARK)
    set(KFR_ENABLE_DFT ON CACHE BOOL "" FORCE)
    else()
        set(KFR_ENABLE_DFT OFF CACHE BOOL "" FORCE)
    endif()
    set(KFR_ENABLE_IO OFF CACHE BOOL "" FORCE)
    if(FIR_ENABLE_KFR_BENCHMARK)
        set(KFR_ENABLE_DSP ON CACHE BOOL "" FORCE)
    else()
        set(KFR_ENABLE_DSP OFF CACHE BOOL "" FORCE)
    endif()
    set(KFR_ENABLE_AUDIO OFF CACHE BOOL "" FORCE)
    set(KFR_ENABLE_CAPI_BUILD OFF CACHE BOOL "" FORCE)
    set(KFR_ENABLE_MULTIARCH OFF CACHE BOOL "" FORCE)
    set(KFR_INSTALL_LIBRARIES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(kfr
        GIT_REPOSITORY https://github.com/kfrlib/kfr.git
        GIT_TAG 6.3.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(kfr)
endif()

if(FFT_ENABLE_KFR_BENCHMARK)
    fft_add_bench_backend(kfr
        ${CMAKE_CURRENT_SOURCE_DIR}/backends/kfr/kfr_backend.cpp
        kfr kfr_dft)
    message(STATUS "KFR benchmark backend enabled (benchmark-only, never shipped).")
endif()

if(FIR_ENABLE_KFR_BENCHMARK)
    fir_add_bench_backend(kfr
        ${CMAKE_CURRENT_SOURCE_DIR}/backends/kfr/fir_kfr_backend.cpp
        kfr kfr_dsp)
    message(STATUS "KFR FIR benchmark backend enabled (benchmark-only, never shipped).")
endif()

# Intel oneMKL: a production-capable backend, fetched on demand. Uses the same
# backend source as the production build (src/backends/mkl), not a bench-only one.
if(FFT_ENABLE_MKL_BENCHMARK)
    include(FetchMKL)
    fft_add_bench_backend(mkl
        ${PROJECT_SOURCE_DIR}/src/backends/mkl/mkl_backend.cpp
        MKL::MKL)
    message(STATUS "MKL benchmark backend enabled (fetched oneMKL).")
endif()

# AMD AOCL-FFTZ: production-capable backend, fetched and built from source.
if(FFT_ENABLE_AOCL_BENCHMARK)
    include(FetchAOCL)
    fft_add_bench_backend(aocl
        ${PROJECT_SOURCE_DIR}/src/backends/aocl/aocl_backend.cpp
        AOCL::FFTZ)
    message(STATUS "AOCL benchmark backend enabled (built AOCL-FFTZ).")
endif()

# Arm CMSIS-DSP: production-capable FFT backend, fetched. Uses the same backend
# source as the production build (portable/host, compiler auto-vectorizes to NEON on Arm).
if(FFT_ENABLE_CMSIS_BENCHMARK)
    include(FetchCMSISDSP)
    fft_add_bench_backend(cmsis
        ${PROJECT_SOURCE_DIR}/src/backends/cmsis/cmsis_backend.cpp
        CMSISDSP::CMSISDSP)
    message(STATUS "CMSIS-DSP FFT benchmark backend enabled (fetched; portable/host, compiler auto-vectorizes to NEON on Arm).")
endif()

# Arm Performance Libraries: production-capable AArch64 FFT backend, fetched on
# demand. Uses the same backend source as the production build (src/backends/armpl).
if(FFT_ENABLE_ARMPL_BENCHMARK)
    include(FetchArmPL)
    fft_add_bench_backend(armpl
        ${PROJECT_SOURCE_DIR}/src/backends/armpl/armpl_backend.cpp
        ArmPL::ArmPL)
    message(STATUS "ArmPL benchmark backend enabled (fetched Arm Performance Libraries).")
endif()

# PocketFFT: production-capable portable C++ FFT backend, fetched (header-only).
# Uses the same backend source as the production build (src/backends/pocketfft).
if(FFT_ENABLE_POCKETFFT_BENCHMARK)
    include(FetchPocketFFT)
    fft_add_bench_backend(pocketfft
        ${PROJECT_SOURCE_DIR}/src/backends/pocketfft/pocketfft_backend.cpp
        PocketFFT::PocketFFT)
    message(STATUS "PocketFFT benchmark backend enabled (fetched header-only).")
endif()

# FFTW: benchmark-only (GPL), fetched and built from source. Single precision
# (fftw3f), complex f32 -- the standard c2c benchmark. Works on x86 and Arm.
# Lives entirely under bench/ and is never linked into the library or installed.
if(FFT_ENABLE_FFTW_BENCHMARK)
    include(FetchFFTW)
    fft_add_bench_backend(fftw
        ${CMAKE_CURRENT_SOURCE_DIR}/backends/fftw/fftw_backend.cpp
        FFTW::FFTW)
    message(STATUS "FFTW benchmark backend enabled (benchmark-only, never shipped).")
endif()

# Aggregate targets: build every FFT (resp. FIR) benchmark executable that was
# actually configured for this host. The Makefile builds these instead of naming
# individual backends, so which backends exist stays entirely CMake's decision.
# The FIR portable backend is always registered; on the FFT side PocketFFT is the
# auto-enabled baseline, so both properties are non-empty for a benchmark build.
get_property(_fft_bench_targets GLOBAL PROPERTY DSP_FFT_BENCH_TARGETS)
add_custom_target(fft_benchmarks DEPENDS ${_fft_bench_targets})
get_property(_fir_bench_targets GLOBAL PROPERTY DSP_FIR_BENCH_TARGETS)
add_custom_target(fir_benchmarks DEPENDS ${_fir_bench_targets})
