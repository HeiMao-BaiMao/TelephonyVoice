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
option(TELEPHONY_USE_EVS_JBM "Build the experimental EVS Stage-1 JBM/VoIP receive adapter (float EVS, non-distribution only)" OFF)
# ---------------------------------------------------------------------------
# Experimental / non-distribution additions
#
# When ON we build SpeexDSP (BSD) and Opus (BSD) into the project and expose
# the new `OPUS_VOIP` mode + the SpeexDSP-backed VAD/DTX/CNG helper. This is
# intentionally OFF under TELEPHONY_DISTRIBUTION_BUILD so shipped binaries
# never link the experimental libraries. Personal / internal builds default
# to ON so the new mode is available out of the box.
# ---------------------------------------------------------------------------
option(TELEPHONY_EXPERIMENTAL_NETWORK "Build SpeexDSP and Opus support and expose the experimental OPUS_VOIP mode (non-distribution builds only)" ON)

if(TELEPHONY_DISTRIBUTION_BUILD AND TELEPHONY_EXPERIMENTAL_NETWORK)
    # The distribution build intentionally avoids the experimental libraries;
    # force the option off rather than letting it sneak in via cache.
    set(TELEPHONY_EXPERIMENTAL_NETWORK OFF CACHE BOOL "" FORCE)
    message(STATUS "TELEPHONY_EXPERIMENTAL_NETWORK forced OFF by TELEPHONY_DISTRIBUTION_BUILD")
endif()

if(TELEPHONY_DISTRIBUTION_BUILD AND TELEPHONY_USE_EVS_FX)
    message(FATAL_ERROR "TELEPHONY_USE_EVS_FX is not available in TELEPHONY_DISTRIBUTION_BUILD")
endif()

if(TELEPHONY_USE_EVS_JBM AND TELEPHONY_DISTRIBUTION_BUILD)
    message(FATAL_ERROR "TELEPHONY_USE_EVS_JBM is not available in TELEPHONY_DISTRIBUTION_BUILD")
endif()

if(TELEPHONY_USE_EVS_JBM AND TELEPHONY_USE_EVS_FX)
    message(FATAL_ERROR "TELEPHONY_USE_EVS_JBM is only implemented for the floating-point EVS variant (TELEPHONY_USE_EVS_FX must be OFF)")
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

# SpeexDSP and Opus are experimental additions. They are BSD-licensed so
# they're safe to ship, but they're not part of the original telephony stack
# so we gate them behind TELEPHONY_EXPERIMENTAL_NETWORK. Distribution builds
# always have that option forced OFF above.
if(TELEPHONY_EXPERIMENTAL_NETWORK)
    include(cmake/speexdsp.cmake)
    include(cmake/opus.cmake)
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
    if(TELEPHONY_USE_EVS_JBM)
        list(APPEND EVS_API_SRC "${CMAKE_CURRENT_SOURCE_DIR}/evs_api_rx.c")
    endif()
    set(EVS_API_LIBS evs-lib-enc evs-lib-dec)
endif()

# ---------------------------------------------------------------------------
# Source-specific forced-include for evs_api_fx.c (parent-only FX build)
#
# cmake/3gpp-evs.cmake generates an FX typedef shim that maps Word16/Word32
# to the fixed-point types, pre-defines the float lib_com/cnst.h guard
# (CNST_H), pre-defines the float lib_com/typedef.h guards (TYPEDEF_H /
# _TYPEDEF_H), and overrides the broken lib_com/control.h FOR/WHILE/DO/IF/
# ELSE/SWITCH/CONTINUE/BREAK/GOTO macros.  This shim is force-included into
# every translation unit of evs-lib-{com,enc,dec}-fx, but the parent-repo
# C wrapper evs_api_fx.c lives in TelephonyDSP - not in those targets -
# so it does not currently see the shim.
#
# Apply the shim to evs_api_fx.c ONLY, never to the rest of TelephonyDSP:
#   - The C++ sources use the all-caps identifiers `FOR`, `WHILE`, etc. in
#     legitimate ways (none today, but new C++ code might), and remapping
#     `Mpy_32_16` -> `evs_fx_Mpy_32_16_2arg` globally would poison anything
#     that touches basic_math/oper_32b.h transitively.
#   - `Word16` / `Word32` / `UWord16` / `UWord32` are FX-only types that
#     must not leak into C++ TUs that include only the float headers.
#
# set_source_files_properties applied to the absolute path used in
# EVS_API_SRC above is the canonical CMake mechanism for this; it must
# happen before add_library(TelephonyDSP ...) so the generator picks up
# the per-file COMPILE_OPTIONS when it writes the build.ninja rule.
#
# EVS_FX_TYPEDEF_SHIM and EVS_FX_INCLUDE_DIRS are both defined inside the
# `if(TELEPHONY_USE_EVS_FX)` block of cmake/3gpp-evs.cmake (which has
# already been included by this point via the `include(cmake/3gpp-evs.cmake)`
# call in the External Libraries section above), so they are visible here.
# Include directories are already propagated transitively from
# evs-lib-{com,enc,dec}-fx (PUBLIC link with PUBLIC include_directories),
# so we do NOT need to add EVS_FX_INCLUDE_DIRS to the source here - we
# only need to attach the force-include option.  We attach it explicitly
# anyway, as a safety net, so the source compiles even if a future
# refactor flips the FX libs to PRIVATE include directories.
# ---------------------------------------------------------------------------
if(TELEPHONY_USE_EVS_FX)
    if(MSVC)
        # MSVC uses /FI<path> (no space).  The path is absolute because
        # EVS_FX_TYPEDEF_SHIM is set to
        # "${CMAKE_CURRENT_BINARY_DIR}/evs_fx_shim/evs_fx_typedef_shim.h".
        set_source_files_properties("${EVS_API_SRC}" PROPERTIES
            INCLUDE_DIRECTORIES "${EVS_FX_INCLUDE_DIRS}"
            COMPILE_OPTIONS    "/FI${EVS_FX_TYPEDEF_SHIM}"
        )
    else()
        # GCC/Clang use -include <path> (with space).  CMake's
        # COMPILE_OPTIONS is a list, so split the option from its value.
        set_source_files_properties("${EVS_API_SRC}" PROPERTIES
            INCLUDE_DIRECTORIES "${EVS_FX_INCLUDE_DIRS}"
            COMPILE_OPTIONS    "-include;${EVS_FX_TYPEDEF_SHIM}"
        )
    endif()
endif()

add_library(TelephonyDSP STATIC
    dsp/RingBuffer.cpp
    dsp/WaveformConcealer.cpp
    dsp/G711Codec.cpp
    dsp/GsmCodec.cpp
    dsp/AmrNbCodec.cpp
    dsp/AmrWbCodec.cpp
    dsp/EvsCodec.cpp
    dsp/EvsCodecJbm.cpp
    dsp/OpusCodec.cpp
    dsp/SpeexDSPAux.cpp
    dsp/ChannelProcessor.cpp
    dsp/SignalProcessor.cpp
    ${EVS_API_SRC}
)

target_link_libraries(TelephonyDSP PUBLIC
    r8brain
    g711-codec
    gsm
    ${EVS_API_LIBS}
)

if(TELEPHONY_USE_EVS_JBM)
    target_compile_definitions(TelephonyDSP PUBLIC TELEPHONY_USE_EVS_JBM=1)
else()
    target_compile_definitions(TelephonyDSP PUBLIC TELEPHONY_USE_EVS_JBM=0)
endif()

# ---------------------------------------------------------------------------
# EVSJbmSmoke (experimental, float EVS only)
#
# Standalone smoke executable that proves the parent-repo evs_api_rx
# adapter can receive compact AU data produced by the existing
# evs_enc_process G.192 short-stream output. It uses only public parent
# APIs (evs_api.h, evs_api_rx.h) and does not wire into TelephonyDSP
# modes/runner/UI.
#
# Built only when TELEPHONY_USE_EVS_JBM=ON (which already implies
# TELEPHONY_USE_EVS_FX=OFF and TELEPHONY_DISTRIBUTION_BUILD=OFF, so the
# float EVS reference libraries are linked and available here).
# ---------------------------------------------------------------------------
if(TELEPHONY_USE_EVS_JBM)
    add_executable(EVSJbmSmoke
        "${CMAKE_CURRENT_SOURCE_DIR}/evs_api_rx_smoke.c"
    )
    target_link_libraries(EVSJbmSmoke PRIVATE TelephonyDSP)
    if(MSVC)
        target_compile_options(EVSJbmSmoke PRIVATE /W3)
        target_compile_definitions(EVSJbmSmoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

if(NOT TELEPHONY_DISTRIBUTION_BUILD)
    target_link_libraries(TelephonyDSP PUBLIC
        vo-amrwbenc
        opencore-amrnb
        opencore-amrwb
    )
else()
    target_compile_definitions(TelephonyDSP PUBLIC TELEPHONY_DISTRIBUTION_BUILD=1)
endif()

# Experimental additions (SpeexDSP, Opus). The OPUS_VOIP / SpeexDSP-backed
# helper is referenced from TelephonyDSP only when this is ON.
if(TELEPHONY_EXPERIMENTAL_NETWORK)
    target_link_libraries(TelephonyDSP PUBLIC speexdsp-core opus-codec)
    target_compile_definitions(TelephonyDSP PUBLIC TELEPHONY_EXPERIMENTAL_NETWORK=1)
else()
    target_compile_definitions(TelephonyDSP PUBLIC TELEPHONY_EXPERIMENTAL_NETWORK=0)
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

# ---------------------------------------------------------------------------
# Tests (ctest)
#
# TelephonyDspSmoke sweeps every EraMode reachable in the current build
# configuration through SignalProcessor and, on non-distribution builds,
# round-trips the raw EVS C API including the DTX SID/NO_DATA path.
#
# Still skipped under TELEPHONY_USE_EVS_FX, but no longer because of the
# linker: the FX variant now builds and links TelephonyRunner.exe with
# LNK1120 = 0 (verified 2026-10-06).  The blocker is runtime: the fixed-point
# core is still mostly link-unblock stubs (ACELP/TCX coding, FD-CNG, LPD
# state machine), and running real audio through it corrupts memory and kills
# the process (mono: 0xC0000409 inside evs_enc_fx after ~30 frames; stereo:
# 0xC0000005 in the second init_decoder_fx).  The smoke test would therefore
# crash instead of reporting a result, so it stays disabled until the core
# port lands.  See README.md and EVS_FX_PROGRESS.md.
# ---------------------------------------------------------------------------
if(NOT TELEPHONY_USE_EVS_FX)
    enable_testing()

    add_executable(TelephonyDspSmoke tests/TelephonyDspSmoke.cpp)
    target_link_libraries(TelephonyDspSmoke PRIVATE TelephonyDSP)
    if(MSVC)
        target_compile_options(TelephonyDspSmoke PRIVATE /W3 /MP)
        target_compile_definitions(TelephonyDspSmoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_test(NAME dsp_smoke COMMAND TelephonyDspSmoke)

    if(TELEPHONY_USE_EVS_JBM)
        add_test(NAME evs_jbm_smoke COMMAND EVSJbmSmoke)
    endif()
endif()
