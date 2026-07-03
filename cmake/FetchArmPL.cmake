# FetchArmPL.cmake
#
# Acquire Arm Performance Libraries (ArmPL) -- headers + the static library --
# without a system install, and expose the imported target ArmPL::ArmPL.
#
# ArmPL is the AArch64 counterpart to Intel oneMKL (x86, see FetchMKL.cmake) and
# AMD AOCL-FFTZ (x86). Its FFT component implements the standard FFTW3 interface
# (the fftwf_*/fftw_* symbols live in libarmpl), which src/backends/armpl uses.
#
# Unlike Intel -- which publishes per-component PyPI wheels (plain zips) -- Arm
# distributes ArmPL only as one large tarball whose payload is a self-extracting
# shell installer wrapping .deb packages. So there is no "just download a zip"
# path; this module reproduces, without root or dpkg, the exact unwrap Arm's
# installer would do, then pulls only the LP64 (sequential) static library and
# headers out of the .deb data payload:
#
#   <tarball>                                      (file DOWNLOAD)
#     -> arm-performance-libraries_<v>_deb/*.sh    (file ARCHIVE_EXTRACT)
#        -> gzip payload after __START_OF_PAYLOAD__ (tail | gzip | tar)
#           -> armpl_<v>_gcc.deb                     (ar p | tar: data.tar.gz)
#              -> opt/arm/armpl_<v>_gcc/{lib,include}
#
# The GCC build (deb_gcc) is selected: its objects use the gfortran LP64 ABI and
# link cleanly with GCC/G++. Building this project with Arm Compiler for Linux
# would instead want the _arm (armflang) build -- override ARMPL_TARBALL_URL.
#
# The static link line is Arm's own recommendation from the shipped examples:
#   -larmpl_lp64 -lamath -lm      (sequential / single-threaded LP64)
# libamath supplies ArmPL's vector-math helpers; the C FFTW3 interface pulls no
# Fortran runtime, so no -lgfortran is needed.

include_guard(GLOBAL)

if(TARGET ArmPL::ArmPL)
    return()
endif()

if(NOT (CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64"))
    message(FATAL_ERROR "ArmPL is an AArch64 library; host is ${CMAKE_SYSTEM_PROCESSOR}.")
endif()

set(ARMPL_VERSION "26.01.1" CACHE STRING "Arm Performance Libraries version")
set(ARMPL_TARBALL_URL
    "https://developer.arm.com/-/cdn-downloads/permalink/Arm-Performance-Libraries/Version_${ARMPL_VERSION}/arm-performance-libraries_${ARMPL_VERSION}_deb_gcc.tar"
    CACHE STRING "URL of the ArmPL deb_gcc tarball (a self-extracting installer, ~0.9 GB)")

set(_armpl_root "${CMAKE_BINARY_DIR}/_armpl")
set(_armpl_prefix "${_armpl_root}/opt/arm/armpl_${ARMPL_VERSION}_gcc")
set(_armpl_marker "${_armpl_root}/.extracted-${ARMPL_VERSION}")

if(NOT EXISTS "${_armpl_marker}")
    # The unwrap needs POSIX tools (tail/gzip/tar/ar) to peel the self-extracting
    # installer and the .deb; ArmPL is Linux-only, so these are always present.
    foreach(_tool bash tar ar)
        find_program(_armpl_${_tool} ${_tool})
        if(NOT _armpl_${_tool})
            message(FATAL_ERROR "ArmPL: '${_tool}' not found; required to unpack the installer.")
        endif()
    endforeach()

    file(MAKE_DIRECTORY "${_armpl_root}")

    # 1. Download the tarball (cached; large).
    set(_armpl_tar "${_armpl_root}/armpl.tar")
    if(NOT EXISTS "${_armpl_tar}")
        message(STATUS "ArmPL: downloading ${ARMPL_VERSION} tarball (~0.9 GB, one time) ...")
        file(DOWNLOAD "${ARMPL_TARBALL_URL}" "${_armpl_tar}" STATUS _st TLS_VERIFY ON SHOW_PROGRESS)
        list(GET _st 0 _code)
        if(NOT _code EQUAL 0)
            file(REMOVE "${_armpl_tar}")
            message(FATAL_ERROR "ArmPL: download failed: ${_st}\n  url: ${ARMPL_TARBALL_URL}")
        endif()
    endif()

    # 2. Extract the outer tar -> arm-performance-libraries_<v>_deb/<installer>.sh
    set(_armpl_unwrap "${_armpl_root}/unwrap")
    file(REMOVE_RECURSE "${_armpl_unwrap}")
    file(MAKE_DIRECTORY "${_armpl_unwrap}")
    message(STATUS "ArmPL: extracting installer tarball ...")
    file(ARCHIVE_EXTRACT INPUT "${_armpl_tar}" DESTINATION "${_armpl_unwrap}")
    file(GLOB_RECURSE _armpl_sh "${_armpl_unwrap}/*_deb.sh")
    if(NOT _armpl_sh)
        message(FATAL_ERROR "ArmPL: installer .sh not found in tarball")
    endif()
    list(GET _armpl_sh 0 _armpl_installer)

    # 3+4. Peel the installer's gzip payload (a tar of .deb files) and extract the
    # LP64 static library + headers straight out of the .deb's data.tar.gz. Done
    # in one shell pipeline so no multi-GB intermediate is ever materialized: only
    # the files we actually link are written to disk.
    message(STATUS "ArmPL: unpacking LP64 static library and headers ...")
    set(_armpl_glob "*/armpl_${ARMPL_VERSION}_gcc")
    execute_process(
        COMMAND ${_armpl_bash} -c "
            set -euo pipefail
            sh='${_armpl_installer}'
            start=$(awk '/^__START_OF_PAYLOAD__/ {print NR + 1; exit}' \"$sh\")
            work=$(mktemp -d)
            trap 'rm -rf \"$work\"' EXIT
            # installer.sh -> gzip payload -> tar of .deb files
            tail -n +$start \"$sh\" | gzip -dc | tar x -C \"$work\"
            deb=$(ls \"$work\"/armpl_*_gcc.deb)
            # .deb (ar) -> data.tar.gz -> selective extract of lib + include only
            ar p \"$deb\" data.tar.gz | tar xz -C '${_armpl_root}' --wildcards \
                '${_armpl_glob}/lib/libarmpl_lp64.a' \
                '${_armpl_glob}/lib/libamath.a' \
                '${_armpl_glob}/lib/libastring.a' \
                '${_armpl_glob}/include/*'
        "
        RESULT_VARIABLE _armpl_rc)
    if(NOT _armpl_rc EQUAL 0)
        message(FATAL_ERROR "ArmPL: failed to unpack the .deb payload (rc=${_armpl_rc}).")
    endif()

    if(NOT EXISTS "${_armpl_prefix}/lib/libarmpl_lp64.a")
        message(FATAL_ERROR "ArmPL: libarmpl_lp64.a not found under ${_armpl_prefix}/lib")
    endif()
    # Reclaim the ~0.9 GB download and the unwrapped installer; keep only lib+include.
    file(REMOVE "${_armpl_tar}")
    file(REMOVE_RECURSE "${_armpl_unwrap}")
    file(TOUCH "${_armpl_marker}")
endif()

if(NOT EXISTS "${_armpl_prefix}/include/fftw3.h")
    message(FATAL_ERROR "ArmPL: fftw3.h not found under ${_armpl_prefix}/include")
endif()

add_library(ArmPL::ArmPL INTERFACE IMPORTED GLOBAL)
target_include_directories(ArmPL::ArmPL INTERFACE "${_armpl_prefix}/include")
target_link_libraries(ArmPL::ArmPL INTERFACE
    "${_armpl_prefix}/lib/libarmpl_lp64.a"
    "${_armpl_prefix}/lib/libamath.a"
    m)

message(STATUS "ArmPL: include dir ${_armpl_prefix}/include")
message(STATUS "ArmPL: static lib ${_armpl_prefix}/lib/libarmpl_lp64.a (sequential LP64)")
