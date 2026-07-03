# FetchCMSISDSP.cmake
#
# Fetch Arm's CMSIS-DSP and expose its static library as CMSISDSP::CMSISDSP for
# the FIR backend. CMSIS-DSP is portable C: it builds a scalar kernel for a host
# CPU (x86_64 lands here via HOST=ON) and a NEON-accelerated kernel on AArch64
# (NEON=ON). Both paths implement the same arm_fir_* API the backend uses, so a
# single backend source compiles for x86 and Arm.

include_guard(GLOBAL)

if(TARGET CMSISDSP::CMSISDSP)
    return()
endif()

set(CMSISDSP_GIT_TAG "v1.17.0" CACHE STRING "CMSIS-DSP git tag to fetch")

# CMSIS-DSP reads these as its own CMake options; they must be set before
# FetchContent_MakeAvailable so its configure step picks them up.
#   HOST=ON  -> portable scalar build for a host CPU (used on x86_64).
#   NEON=ON  -> Arm NEON acceleration on AArch64.
#   DISABLEFLOAT16=ON -> skip float16 kernels; they need _Float16 support that
#                        host toolchains frequently lack, and the FIR backend
#                        never touches them.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|armv8|armv7")
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

if(HOST)
    message(STATUS "CMSIS-DSP: host/scalar build (${CMSISDSP_GIT_TAG})")
else()
    message(STATUS "CMSIS-DSP: NEON build (${CMSISDSP_GIT_TAG})")
endif()
