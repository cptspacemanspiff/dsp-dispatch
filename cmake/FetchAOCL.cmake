# FetchAOCL.cmake
#
# Build AMD AOCL-FFTZ (the AMD "Zen" FFT library, analogous to oneMKL's DFT)
# from source via FetchContent and expose it as the imported target AOCL::FFTZ.
#
# AOCL-FFTZ is a CMake source project, so unlike oneMKL we compile it here into a
# static library (BUILD_STATIC_LIBS=ON) rather than fetching prebuilt binaries.
# Single-threaded by default (ENABLE_MULTI_THREADING=OFF), matching the rest of
# the benchmark setup. The library is Apache-2.0 / MIT-style permissive.

include_guard(GLOBAL)

if(TARGET AOCL::FFTZ)
    return()
endif()

# AOCL-FFTZ is C; make sure C is enabled in case the parent project is CXX-only.
enable_language(C)

include(FetchContent)

set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)       # static, not shared
set(ENABLE_MULTI_THREADING OFF CACHE BOOL "" FORCE) # single-threaded
set(ENABLE_STRICT_WARNINGS OFF CACHE BOOL "" FORCE) # don't -Werror their tree on our compiler
set(AOCL_TEST_COVERAGE OFF CACHE STRING "" FORCE)   # no GTest/CTest subtree
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_THIRD_PARTY_WRAPPERS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(aoclfftz
    GIT_REPOSITORY https://github.com/amd/aocl-fftz
    GIT_TAG 5.3
    GIT_SHALLOW TRUE
    # AOCL-FFTZ defines a global custom target named "uninstall", which collides
    # with the same-named target from other fetched projects (e.g. KFR). Rename
    # it so multiple fetched backends can coexist in one build.
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/PatchAoclUninstall.cmake)
FetchContent_MakeAvailable(aoclfftz)

# The static library target is named aocl_fftz on Linux/macOS and
# aocl_fftz_static on Windows; link whichever this build produced.
if(TARGET aocl_fftz)
    set(_aocl_target aocl_fftz)
elseif(TARGET aocl_fftz_static)
    set(_aocl_target aocl_fftz_static)
else()
    message(FATAL_ERROR "AOCL-FFTZ: expected static target not found after fetch")
endif()

add_library(AOCL::FFTZ INTERFACE IMPORTED GLOBAL)
target_link_libraries(AOCL::FFTZ INTERFACE ${_aocl_target})
# Public headers (aoclfftz.h) live under api/; the project uses include_directories(.)
# internally and does not export them, so add it explicitly for consumers.
target_include_directories(AOCL::FFTZ INTERFACE "${aoclfftz_SOURCE_DIR}/api")

message(STATUS "AOCL-FFTZ: built static (aocl_fftz_static), headers ${aoclfftz_SOURCE_DIR}/api")
