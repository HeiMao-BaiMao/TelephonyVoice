cmake_minimum_required(VERSION 3.15)

project(TelephonyVoice VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 3GPP EVS codec variant selection
# Default: floating-point reference (TS 26.443 v12.7.0 / v13.3.0)
# Set TELEPHONY_USE_EVS_FX=ON to build the fixed-point variant
# (TS 26.442 v12.15.0 / v13.10.0 / v14.6.0 / v15.4.0 / v16.4.0) instead.
# ---------------------------------------------------------------------------
option(TELEPHONY_USE_EVS_FX "Use 3GPP EVS fixed-point reference (v16.4.0) instead of floating-point (v13.3.0)" OFF)
option(TELEPHONY_DISTRIBUTION_BUILD "Strip AMR/AMR-WB/EVS reference codecs and use distributable stand-ins only" OFF)

if(TELEPHONY_DISTRIBUTION_BUILD AND TELEPHONY_USE_EVS_FX)
    message(FATAL_ERROR "TELEPHONY_USE_EVS_FX is not available in TELEPHONY_DISTRIBUTION_BUILD")
endif()

# ---------------------------------------------------------------------------
# VST3 SDK Configuration
# ---------------------------------------------------------------------------
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "Disable examples" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "Disable hosting examples" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "Enable VSTGUI" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "Disable symbolic link creation" FORCE)
set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "Disable moduleinfo.json creation" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "Disable VST validator" FORCE)

add_subdirectory(external/vst3sdk)

# ---------------------------------------------------------------------------
# External Libraries
# ---------------------------------------------------------------------------
include(cmake/r8brain.cmake)
include(cmake/g711.cmake)
include(cmake/libgsm.cmake)

if(NOT TELEPHONY_DISTRIBUTION_BUILD)
    include(cmake/vo-amrwbenc.cmake)
    include(cmake/opencore-amr.cmake)
    include(cmake/3gpp-evs.cmake)
endif()

# ---------------------------------------------------------------------------
# TelephonyDSP Library (Shared Logic)
# ---------------------------------------------------------------------------
if(TELEPHONY_DISTRIBUTION_BUILD)
    set(EVS_API_SRC "")
    set(EVS_API_LIBS "")
elseif(TELEPHONY_USE_EVS_FX)
    set(EVS_API_SRC "${CMAKE_CURRENT_SOURCE_DIR}/evs_api_fx.c")
    set(EVS_API_LIBS evs-lib-enc-fx evs-lib-dec-fx)
else()
    set(EVS_API_SRC "${CMAKE_CURRENT_SOURCE_DIR}/evs_api.c")
    set(EVS_API_LIBS evs-lib-enc evs-lib-dec)
endif()

add_library(TelephonyDSP STATIC
    TelephonyDSP.cpp
    ${EVS_API_SRC}
)

target_link_libraries(TelephonyDSP PUBLIC
    r8brain
    g711-codec
    gsm
    ${EVS_API_LIBS}
)

if(NOT TELEPHONY_DISTRIBUTION_BUILD)
    target_link_libraries(TelephonyDSP PUBLIC
        vo-amrwbenc
        opencore-amrnb
        opencore-amrwb
    )
else()
    target_compile_definitions(TelephonyDSP PUBLIC TELEPHONY_DISTRIBUTION_BUILD=1)
endif()
# Ensure headers are found
target_include_directories(TelephonyDSP PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")


# ---------------------------------------------------------------------------
# TelephonyVoice VST3 Plugin
# ---------------------------------------------------------------------------
smtg_add_vst3plugin(TelephonyVoice
    TelephonyVoice.cpp
    TelephonyVoice.h
)

target_link_libraries(TelephonyVoice
    PRIVATE
    TelephonyDSP
    sdk             # VST3 SDK Base
    vstgui_support
)

target_include_directories(TelephonyVoice PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/vst3sdk")

if(MSVC)
    target_compile_options(TelephonyVoice PRIVATE /W3 /MP)
endif()

# Fix for vo-amrwbenc crash in Debug mode:
# vo-amrwbenc requires optimization /O2 to avoid stack corruption crashes (likely due to internal buffer overflows hidden by optimization).
# However, /O2 is incompatible with /RTC1 (default in Debug).
# We strip /RTC1 from the global debug flags to allow per-target /O2 usage.
if(MSVC)
    foreach(flag_var
        CMAKE_C_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_DEBUG
    )
        if(${flag_var} MATCHES "/RTC1")
            string(REPLACE "/RTC1" "" ${flag_var} "${${flag_var}}")
        elseif(${flag_var} MATCHES "/RTCsu")
             string(REPLACE "/RTCsu" "" ${flag_var} "${${flag_var}}")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# TelephonyRunner (Sample EXE)
# ---------------------------------------------------------------------------
set(WAV_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/opencore-amr/test")

add_executable(TelephonyRunner
    TelephonyRunner.cpp
    "${WAV_TOOLS_DIR}/wavreader.c"
    "${WAV_TOOLS_DIR}/wavwriter.c"
)

target_include_directories(TelephonyRunner PRIVATE "${WAV_TOOLS_DIR}")
target_link_libraries(TelephonyRunner PRIVATE TelephonyDSP)

if(MSVC)
    target_compile_options(TelephonyRunner PRIVATE /W3 /MP)
    target_compile_definitions(TelephonyRunner PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
