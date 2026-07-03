# FetchIPP.cmake
#
# Locate Intel IPP or install Intel's PyPI packages with uv, then expose the
# minimal static signal-processing target used by the FIR backend as IPP::IPP.

include_guard(GLOBAL)

if(TARGET IPP::IPP)
    return()
endif()

set(IPP_WHEEL_VERSION "2026.0.1" CACHE STRING "Intel IPP PyPI wheel version")
set(IPP_FETCH_WITH_UV ON CACHE BOOL "Install ipp-devel and ipp-static with uv if IPP is not already found")

# Prefer static IPP. The ipp-static wheel provides libipps.a, libippvm.a, and
# libippcore.a; IPP's package config resolves the component dependencies.
set(IPP_SHARED OFF)

if(DEFINED IPP_ROOT AND NOT IPP_ROOT STREQUAL "")
    list(PREPEND CMAKE_PREFIX_PATH "${IPP_ROOT}")
endif()

find_package(IPP CONFIG QUIET COMPONENTS ipps)

if(NOT IPP_FOUND AND IPP_FETCH_WITH_UV)
    find_program(UV_EXECUTABLE uv REQUIRED)
    set(_ipp_root "${CMAKE_BINARY_DIR}/_ipp")
    set(_ipp_marker "${_ipp_root}/lib/libipps.a")
    if(NOT EXISTS "${_ipp_marker}")
        message(STATUS "IPP: installing ipp-devel + ipp-static ${IPP_WHEEL_VERSION} with uv ...")
        execute_process(
            COMMAND "${UV_EXECUTABLE}" pip install
                    --target "${_ipp_root}"
                    "ipp-devel==${IPP_WHEEL_VERSION}"
                    "ipp-static==${IPP_WHEEL_VERSION}"
            RESULT_VARIABLE _ipp_uv_result)
        if(NOT _ipp_uv_result EQUAL 0)
            message(FATAL_ERROR "IPP: uv failed to install ipp-devel/ipp-static")
        endif()
    endif()
    set(IPP_DIR "${_ipp_root}/lib/cmake/ipp" CACHE PATH "Intel IPP package config directory" FORCE)
    find_package(IPP CONFIG REQUIRED COMPONENTS ipps)
endif()

if(NOT IPP_FOUND OR NOT TARGET IPP::ipps)
    message(FATAL_ERROR
        "IPP: could not find component ipps. Set IPP_ROOT/IPP_DIR to an IPP "
        "install, or keep IPP_FETCH_WITH_UV=ON so cmake can install ipp-static.")
endif()

add_library(IPP::IPP INTERFACE IMPORTED GLOBAL)
target_link_libraries(IPP::IPP INTERFACE IPP::ipps)
message(STATUS "IPP: using static ipps component for FIR backend")
