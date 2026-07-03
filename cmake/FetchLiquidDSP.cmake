include(FetchContent)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SANDBOX OFF CACHE BOOL "" FORCE)
set(BUILD_AUTOTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(liquid_dsp
    GIT_REPOSITORY https://github.com/jgaeddert/liquid-dsp.git
    GIT_TAG v1.7.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(liquid_dsp)

if(TARGET liquid)
    set(_liquid_dsp_target liquid)
elseif(TARGET liquid-dsp)
    set(_liquid_dsp_target liquid-dsp)
elseif(TARGET liquid_static)
    set(_liquid_dsp_target liquid_static)
else()
    message(FATAL_ERROR "liquid-dsp was fetched, but no known CMake target was created.")
endif()

if(NOT TARGET LiquidDSP::LiquidDSP)
    add_library(liquid_dsp_interface INTERFACE)
    target_link_libraries(liquid_dsp_interface INTERFACE ${_liquid_dsp_target})
    target_include_directories(liquid_dsp_interface SYSTEM INTERFACE
        ${liquid_dsp_SOURCE_DIR}/include)
    add_library(LiquidDSP::LiquidDSP ALIAS liquid_dsp_interface)
endif()
