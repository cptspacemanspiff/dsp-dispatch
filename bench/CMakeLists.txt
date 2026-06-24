# Per-backend benchmark executables.
#
# Each benchmarked backend becomes its own static library (shared common code +
# that backend's BackendPlan implementation) and its own executable built from
# the single backend-agnostic harness (bench_fft.cpp). This is what lets you run
# `fft_bench_portable` and `fft_bench_kfr` side by side from one build.

# fft_add_bench_backend(<name> <backend_source>)
# Extra args after the source are passed to target_link_libraries on the lib.
function(fft_add_bench_backend name backend_src)
    add_library(fft_dispatch_${name} STATIC
        ${PROJECT_SOURCE_DIR}/src/common/fft.cpp
        ${backend_src})
    target_include_directories(fft_dispatch_${name}
        PUBLIC ${PROJECT_SOURCE_DIR}/include
        PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(fft_dispatch_${name} PUBLIC cxx_std_17)
    if(ARGN)
        target_link_libraries(fft_dispatch_${name} PUBLIC ${ARGN})
    endif()

    add_executable(fft_bench_${name} ${CMAKE_CURRENT_SOURCE_DIR}/bench_fft.cpp)
    target_link_libraries(fft_bench_${name}
        PRIVATE fft_dispatch_${name} benchmark::benchmark)
    target_include_directories(fft_bench_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

# Portable: always available, no external dependencies.
fft_add_bench_backend(portable
    ${PROJECT_SOURCE_DIR}/src/backends/portable/portable_backend.cpp)

# KFR: benchmark-only, fetched on demand. Lives entirely under bench/.
if(FFT_ENABLE_KFR_BENCHMARK)
    include(FetchContent)
    set(KFR_ENABLE_DFT ON CACHE BOOL "" FORCE)
    set(KFR_ENABLE_IO OFF CACHE BOOL "" FORCE)
    set(KFR_ENABLE_DSP OFF CACHE BOOL "" FORCE)
    set(KFR_ENABLE_AUDIO OFF CACHE BOOL "" FORCE)
    set(KFR_ENABLE_CAPI_BUILD OFF CACHE BOOL "" FORCE)
    set(KFR_ENABLE_MULTIARCH OFF CACHE BOOL "" FORCE)
    set(KFR_INSTALL_LIBRARIES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(kfr
        GIT_REPOSITORY https://github.com/kfrlib/kfr.git
        GIT_TAG 6.3.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(kfr)

    fft_add_bench_backend(kfr
        ${CMAKE_CURRENT_SOURCE_DIR}/backends/kfr/kfr_backend.cpp
        kfr kfr_dft)
    message(STATUS "KFR benchmark backend enabled (benchmark-only, never shipped).")
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
