# FetchCMSISDSP.cmake
#
# Fetch Arm's CMSIS-DSP and expose its static library as CMSISDSP::CMSISDSP for
# the FFT and FIR backends. CMSIS-DSP is portable C; a single backend source
# compiles for x86 and Arm against the same arm_cfft_*/arm_rfft_*/arm_fir_* API.

include_guard(GLOBAL)

if(TARGET CMSISDSP::CMSISDSP)
    return()
endif()

set(CMSISDSP_GIT_TAG "v1.17.0" CACHE STRING "CMSIS-DSP git tag to fetch")

# CMSIS-DSP build options. These must be set before FetchContent_MakeAvailable
# so its configure step picks them up.
#
# HOST=ON builds the portable scalar kernels and (crucially) defines
# __GNUC_PYTHON__, which makes arm_math_types.h skip its `#include
# "cmsis_compiler.h"` -- that header lives in CMSIS-Core, a *separate* repo. So
# HOST=ON is dependency-free and builds on any CPU. It is arch-independent: on
# aarch64 the compiler still auto-vectorizes these C kernels to NEON at -O3
# (NEON is baseline on armv8-a), so we get SIMD without pulling CMSIS-Core.
#
# CMSIS-DSP's hand-written NEON *intrinsic* kernels (NEON=ON) additionally
# require CMSIS-Core headers. To opt in, set -DCMSISDSP_USE_NEON=ON together with
# -DCMSISCORE=<path to a CMSIS/Core checkout that provides cmsis_compiler.h>.
#
# DISABLEFLOAT16=ON skips float16 kernels; they need _Float16 support that host
# toolchains frequently lack, and neither backend touches them.
option(CMSISDSP_USE_NEON "Use CMSIS-DSP's NEON intrinsic kernels (requires CMSISCORE)" OFF)
if(CMSISDSP_USE_NEON)
    if(NOT DEFINED CMSISCORE)
        message(FATAL_ERROR
            "CMSISDSP_USE_NEON=ON requires -DCMSISCORE=<path to CMSIS Core> so "
            "CMSIS-DSP can find cmsis_compiler.h.")
    endif()
    set(NEON ON CACHE BOOL "" FORCE)
    set(HOST OFF CACHE BOOL "" FORCE)
else()
    set(HOST ON CACHE BOOL "" FORCE)
    set(NEON OFF CACHE BOOL "" FORCE)
endif()
set(DISABLEFLOAT16 ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(cmsisdsp
    GIT_REPOSITORY https://github.com/ARM-software/CMSIS-DSP.git
    GIT_TAG ${CMSISDSP_GIT_TAG}
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(cmsisdsp)

if(NOT TARGET CMSISDSP::CMSISDSP)
    if(TARGET CMSISDSP)
        add_library(CMSISDSP::CMSISDSP ALIAS CMSISDSP)
    else()
        message(FATAL_ERROR
            "CMSIS-DSP was fetched but neither CMSISDSP::CMSISDSP nor CMSISDSP "
            "targets were created.")
    endif()
endif()

if(NEON)
    message(STATUS "CMSIS-DSP: NEON-intrinsic build (${CMSISDSP_GIT_TAG})")
else()
    message(STATUS "CMSIS-DSP: portable/host build, compiler auto-vectorized (${CMSISDSP_GIT_TAG})")
endif()
