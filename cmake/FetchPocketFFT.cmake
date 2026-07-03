# FetchPocketFFT.cmake
#
# Acquire PocketFFT (mreineck/pocketfft, the header-only C++ variant) and expose
# it as the imported target PocketFFT::PocketFFT for src/backends/pocketfft.
#
# PocketFFT is a single self-contained header with no build system of its own, so
# -- unlike the FetchContent-based fetches -- this just downloads that one header,
# pinned to an immutable commit URL and verified by SHA256 (fully reproducible, no
# git checkout, and none of PocketFFT's own CMake/Python tooling is pulled in).
#
# BSD-3-Clause, portable C++: the same header builds on x86 and AArch64, so the
# pocketfft backend is a production-capable, redistributable fallback on every
# host.

include_guard(GLOBAL)

if(TARGET PocketFFT::PocketFFT)
    return()
endif()

set(POCKETFFT_COMMIT "c90e55b3d529f8efa40ed01a20de22405f45fc65"
    CACHE STRING "PocketFFT (cpp branch) commit to pin")
set(POCKETFFT_SHA256 "3e9a05318d8e3b1446bda1c4617e6a103cdd23599ae0a776a92a6e8800e92fdc"
    CACHE STRING "SHA256 of pocketfft_hdronly.h at POCKETFFT_COMMIT")

set(_pocketfft_dir "${CMAKE_BINARY_DIR}/_pocketfft")
set(_pocketfft_hdr "${_pocketfft_dir}/pocketfft_hdronly.h")

if(NOT EXISTS "${_pocketfft_hdr}")
    set(_url
        "https://raw.githubusercontent.com/mreineck/pocketfft/${POCKETFFT_COMMIT}/pocketfft_hdronly.h")
    message(STATUS "PocketFFT: downloading header (${POCKETFFT_COMMIT}) ...")
    file(DOWNLOAD "${_url}" "${_pocketfft_hdr}"
        EXPECTED_HASH SHA256=${POCKETFFT_SHA256}
        TLS_VERIFY ON
        STATUS _st)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_pocketfft_hdr}")
        message(FATAL_ERROR "PocketFFT: download failed: ${_st}\n  url: ${_url}")
    endif()
endif()

add_library(PocketFFT::PocketFFT INTERFACE IMPORTED GLOBAL)
target_include_directories(PocketFFT::PocketFFT INTERFACE "${_pocketfft_dir}")

message(STATUS "PocketFFT: header-only include ${_pocketfft_dir}")
