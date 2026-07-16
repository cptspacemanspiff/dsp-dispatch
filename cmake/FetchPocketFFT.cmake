# FetchPocketFFT.cmake
#
# Acquire PocketFFT (mreineck/pocketfft, the header-only C++ variant) and expose
# it as the imported target PocketFFT::PocketFFT for src/backends/pocketfft.
#
# PocketFFT is a single self-contained header with no build system of its own, so
# -- unlike the FetchContent-based fetches -- there is nothing to build: we just
# need the one header, pinned to an immutable commit and verified by SHA256 (fully
# reproducible, no git checkout, none of PocketFFT's own CMake/Python tooling).
#
# BSD-3-Clause, portable C++: the same header builds on x86 and AArch64, so the
# pocketfft backend is a production-capable, redistributable fallback on every
# host. Because it is redistributable, the pinned header is VENDORED in-tree under
# third_party/pocketfft/ so the default build is hermetic and needs no network --
# essential under Bazel/rules_foreign_cc, whose sandbox has no network access. The
# download path below remains only as a fallback (and as the documented way to
# refresh the vendored copy when the pin is bumped).

include_guard(GLOBAL)

if(TARGET PocketFFT::PocketFFT)
    return()
endif()

set(POCKETFFT_COMMIT "c90e55b3d529f8efa40ed01a20de22405f45fc65"
    CACHE STRING "PocketFFT (cpp branch) commit to pin")
set(POCKETFFT_SHA256 "3e9a05318d8e3b1446bda1c4617e6a103cdd23599ae0a776a92a6e8800e92fdc"
    CACHE STRING "SHA256 of pocketfft_hdronly.h at POCKETFFT_COMMIT")

# The vendored header lives alongside the project source, so it travels with the
# checkout into any sandbox. This file is cmake/, so the tree root is one level up.
set(_pocketfft_vendored
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/pocketfft/pocketfft_hdronly.h")

set(_pocketfft_dir "${CMAKE_BINARY_DIR}/_pocketfft")
set(_pocketfft_hdr "${_pocketfft_dir}/pocketfft_hdronly.h")

if(EXISTS "${_pocketfft_vendored}")
    # Hermetic path: verify the vendored header matches the pin, then use it in
    # place -- no copy, no network.
    file(SHA256 "${_pocketfft_vendored}" _vendored_sha)
    if(NOT _vendored_sha STREQUAL POCKETFFT_SHA256)
        message(FATAL_ERROR
            "PocketFFT: vendored header hash mismatch.\n"
            "  file:     ${_pocketfft_vendored}\n"
            "  expected: ${POCKETFFT_SHA256}\n"
            "  actual:   ${_vendored_sha}\n"
            "Re-vendor the header for commit ${POCKETFFT_COMMIT}, or update "
            "POCKETFFT_COMMIT/POCKETFFT_SHA256 to match.")
    endif()
    get_filename_component(_pocketfft_dir "${_pocketfft_vendored}" DIRECTORY)
    message(STATUS "PocketFFT: using vendored header ${_pocketfft_vendored}")
elseif(NOT EXISTS "${_pocketfft_hdr}")
    set(_url
        "https://raw.githubusercontent.com/mreineck/pocketfft/${POCKETFFT_COMMIT}/pocketfft_hdronly.h")
    message(STATUS "PocketFFT: vendored header absent; downloading (${POCKETFFT_COMMIT}) ...")
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
