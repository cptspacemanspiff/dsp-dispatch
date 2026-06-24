# FetchMKL.cmake
#
# Acquire Intel oneMKL (headers + static libraries) without a system install by
# downloading Intel's official PyPI wheels -- which are plain zip archives --
# extracting them, and exposing the imported target MKL::MKL.
#
# Headers and libraries are located by globbing the extracted tree, so the exact
# internal wheel layout does not matter. Wheels are version-specific: override
# the *_URL cache variables to use another version, architecture, or a local
# mirror. The default wheels are linux x86_64 (manylinux_2_28).
#
# Links the static, sequential (single-threaded) MKL configuration, which is the
# most reproducible for benchmarking and avoids pulling an OpenMP runtime.

include_guard(GLOBAL)

if(TARGET MKL::MKL)
    return()
endif()

set(MKL_WHEEL_VERSION "2026.0.0" CACHE STRING "Intel oneMKL wheel version (informational)")
set(MKL_STATIC_URL
    "https://files.pythonhosted.org/packages/50/38/3e2575dbf05008a017768669f517a17b0eb0d38dbdb5e09aaf9b6a27b753/mkl_static-2026.0.0-py2.py3-none-manylinux_2_28_x86_64.whl"
    CACHE STRING "URL of the mkl-static wheel (a zip archive)")
set(MKL_INCLUDE_URL
    "https://files.pythonhosted.org/packages/99/b6/16be9aeb8a68a7a1b4c854c6055b51a842d9b37c2f2b84b690f219cb4f4b/mkl_include-2026.0.0-py2.py3-none-manylinux_2_28_x86_64.whl"
    CACHE STRING "URL of the mkl-include wheel (a zip archive)")

set(_mkl_root "${CMAKE_BINARY_DIR}/_mkl")

# Download <url> and extract it into <root>/<tag>, once (cached via a marker).
function(_mkl_fetch tag url)
    set(_marker "${_mkl_root}/${tag}/.extracted")
    if(EXISTS "${_marker}")
        return()
    endif()
    set(_whl "${_mkl_root}/${tag}.whl")
    message(STATUS "MKL: downloading ${tag} wheel (this can be large) ...")
    file(DOWNLOAD "${url}" "${_whl}" STATUS _st TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_whl}")
        message(FATAL_ERROR "MKL: failed to download ${tag}: ${_st}\n  url: ${url}")
    endif()
    file(MAKE_DIRECTORY "${_mkl_root}/${tag}")
    message(STATUS "MKL: extracting ${tag} wheel ...")
    file(ARCHIVE_EXTRACT INPUT "${_whl}" DESTINATION "${_mkl_root}/${tag}")
    file(REMOVE "${_whl}")
    file(TOUCH "${_marker}")
endfunction()

_mkl_fetch(include "${MKL_INCLUDE_URL}")
_mkl_fetch(static "${MKL_STATIC_URL}")

# Locate the DFTI header and derive its include directory.
file(GLOB_RECURSE _mkl_dfti "${_mkl_root}/include/mkl_dfti.h")
if(NOT _mkl_dfti)
    message(FATAL_ERROR "MKL: mkl_dfti.h not found under ${_mkl_root}/include")
endif()
list(GET _mkl_dfti 0 _dfti0)
get_filename_component(MKL_INCLUDE_DIR "${_dfti0}" DIRECTORY)

# Locate the static sequential link trio.
function(_mkl_find_lib outvar libname)
    file(GLOB_RECURSE _hits "${_mkl_root}/static/lib${libname}.a")
    if(NOT _hits)
        message(FATAL_ERROR "MKL: lib${libname}.a not found in the static wheel")
    endif()
    list(GET _hits 0 _h0)
    set(${outvar} "${_h0}" PARENT_SCOPE)
endfunction()

_mkl_find_lib(_MKL_LP64 mkl_intel_lp64)
_mkl_find_lib(_MKL_SEQ mkl_sequential)
_mkl_find_lib(_MKL_CORE mkl_core)

find_package(Threads REQUIRED)

add_library(MKL::MKL INTERFACE IMPORTED GLOBAL)
target_include_directories(MKL::MKL INTERFACE "${MKL_INCLUDE_DIR}")
# Static MKL libraries are mutually recursive, hence the link group.
target_link_libraries(MKL::MKL INTERFACE
    "-Wl,--start-group"
    "${_MKL_LP64}" "${_MKL_SEQ}" "${_MKL_CORE}"
    "-Wl,--end-group"
    Threads::Threads ${CMAKE_DL_LIBS} m)

message(STATUS "MKL: include dir ${MKL_INCLUDE_DIR}")
message(STATUS "MKL: static libs (sequential) ${_MKL_LP64}")
