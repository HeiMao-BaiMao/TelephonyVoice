cmake_minimum_required(VERSION 3.25)

project(TelephonyVoice VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# DSP static libraries are also linked into a shared VST3 module on Unix.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
option(TELEPHONY_BUILD_PLUGIN "Build the VST3 plugin and its graphical editor" ON)
set(SMTG_ENABLE_VSTGUI_SUPPORT ${TELEPHONY_BUILD_PLUGIN} CACHE BOOL "Enable VSTGUI" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "Disable symbolic link creation" FORCE)
set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "Disable moduleinfo.json creation" FORCE)
option(TELEPHONY_VALIDATE_PLUGIN "Run the SDK validator after linking the VST3 plugin" ON)
set(SMTG_RUN_VST_VALIDATOR ${TELEPHONY_VALIDATE_PLUGIN} CACHE BOOL "Validate VST3 plugin" FORCE)

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

set_source_files_properties(dsp/OpusCodec.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/external/opus/celt")

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
    dsp/AdvancedControls.h
    dsp/AudioSimulation.cpp
    dsp/NetworkProfile.cpp
    dsp/Transport.cpp
    dsp/TransportEvs.cpp
    dsp/ChannelTransport.cpp
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
if(TELEPHONY_BUILD_PLUGIN)
# The SDK's third-party setup must run in the target's scope: it supplies the
# platform entry point and bundle layout. Keep its directory-wide flags/output
# paths scoped to the plugin so CLI and regression targets retain their layout.
function(telephony_add_plugin)
get_directory_property(_compile_options COMPILE_OPTIONS)
get_directory_property(_compile_definitions COMPILE_DEFINITIONS)
get_directory_property(_link_libraries LINK_LIBRARIES)
smtg_enable_vst3_sdk()
smtg_add_vst3plugin(TelephonyVoice
    TelephonyVoice.cpp
    TelephonyVoice.h
    TelephonyEditor.cpp
    TelephonyEditor.h
)

smtg_target_add_plugin_resources(TelephonyVoice
    RESOURCES resources/telephonyvoice.uidesc
)

target_link_libraries(TelephonyVoice
    PRIVATE
    TelephonyDSP
    sdk             # VST3 SDK Base
    vstgui_support
)

target_include_directories(TelephonyVoice PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/vst3sdk")

if(MSVC)
    target_compile_options(TelephonyVoice PRIVATE /W3 /MP /fp:precise)
endif()

# add_compile_options/definitions and link_libraries are directory properties,
# unlike ordinary CMake variables. Restore them explicitly for later targets.
set_directory_properties(PROPERTIES
    COMPILE_OPTIONS "${_compile_options}"
    COMPILE_DEFINITIONS "${_compile_definitions}"
    LINK_LIBRARIES "${_link_libraries}")
endfunction()
telephony_add_plugin()
endif() # TELEPHONY_BUILD_PLUGIN

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

if(NOT TELEPHONY_DISTRIBUTION_BUILD)
    target_sources(opencore-amrnb PRIVATE
        dsp/AudioSimulationVad2.cpp
        dsp/AudioSimulationVad2Core.cpp
        dsp/AudioSimulationVad2Fft.cpp)
    target_compile_definitions(TelephonyDSP PRIVATE TELEPHONY_HAVE_VAD2=1)
endif()

# ---------------------------------------------------------------------------
# TelephonyRunner (Sample EXE)
# ---------------------------------------------------------------------------
add_executable(TelephonyRunner
    TelephonyRunner.cpp
)

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
# The same application regressions run for the complete official FX source
# path. Optional reference tools add byte-for-byte synthetic differential tests.
# ---------------------------------------------------------------------------
if(BUILD_TESTING OR NOT DEFINED BUILD_TESTING)
    enable_testing()

    add_executable(TelephonyDspSmoke tests/TelephonyDspSmoke.cpp)
    target_link_libraries(TelephonyDspSmoke PRIVATE TelephonyDSP)
    if(MSVC)
        target_compile_options(TelephonyDspSmoke PRIVATE /W3 /MP)
        target_compile_definitions(TelephonyDspSmoke PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_test(NAME dsp_smoke COMMAND TelephonyDspSmoke)

    add_executable(TelephonyAdvancedEdgeCases tests/AdvancedEdgeCases.cpp)
    target_link_libraries(TelephonyAdvancedEdgeCases PRIVATE TelephonyDSP)
    add_test(NAME advanced_edge_cases COMMAND TelephonyAdvancedEdgeCases)
    set_tests_properties(advanced_edge_cases PROPERTIES TIMEOUT 180)

    add_executable(TelephonyAdvancedIntegration tests/AdvancedIntegration.cpp)
    target_link_libraries(TelephonyAdvancedIntegration PRIVATE TelephonyDSP)
    add_test(NAME advanced_integration COMMAND TelephonyAdvancedIntegration)
    set_tests_properties(advanced_integration PROPERTIES TIMEOUT 180)

    add_executable(TelephonyCodecControls tests/TelephonyCodecControls.cpp)
    target_link_libraries(TelephonyCodecControls PRIVATE TelephonyDSP)
    add_test(NAME codec_controls COMMAND TelephonyCodecControls)
    set_tests_properties(codec_controls PROPERTIES TIMEOUT 120)

    add_executable(TelephonyTransportTests tests/TransportTests.cpp)
    target_link_libraries(TelephonyTransportTests PRIVATE TelephonyDSP)
    add_test(NAME transport_protocols COMMAND TelephonyTransportTests)
    add_executable(TelephonyTransportEvsTests tests/TransportEvsTests.cpp)
    target_link_libraries(TelephonyTransportEvsTests PRIVATE TelephonyDSP)
    add_test(NAME transport_evs COMMAND TelephonyTransportEvsTests)
    add_executable(TelephonyTransportIntegrationTests tests/TransportIntegrationTests.cpp)
    target_link_libraries(TelephonyTransportIntegrationTests PRIVATE TelephonyDSP)
    add_test(NAME transport_integration COMMAND TelephonyTransportIntegrationTests)
    set_tests_properties(transport_protocols transport_evs transport_integration PROPERTIES TIMEOUT 120)
    add_executable(TelephonyTransportBatchTests tests/TransportBatchTests.cpp)
    target_link_libraries(TelephonyTransportBatchTests PRIVATE TelephonyDSP)
    add_test(NAME transport_batch_ingress COMMAND TelephonyTransportBatchTests)
    set_tests_properties(transport_batch_ingress PROPERTIES TIMEOUT 120)
    if(NOT TELEPHONY_DISTRIBUTION_BUILD)
        add_executable(TelephonyTransportAmrCodecTests tests/TransportAmrCodecTests.cpp)
        target_link_libraries(TelephonyTransportAmrCodecTests PRIVATE TelephonyDSP)
        add_test(NAME transport_amr_codecs COMMAND TelephonyTransportAmrCodecTests)
        set_tests_properties(transport_amr_codecs PROPERTIES TIMEOUT 120)
    endif()

    add_executable(TelephonyAudioSimulationTests tests/AudioSimulationTests.cpp)
    target_link_libraries(TelephonyAudioSimulationTests PRIVATE TelephonyDSP)
    if(NOT TELEPHONY_DISTRIBUTION_BUILD)
        target_compile_definitions(TelephonyAudioSimulationTests PRIVATE TELEPHONY_HAVE_VAD2=1)
    endif()
    add_test(NAME audio_simulation COMMAND TelephonyAudioSimulationTests)
    set_tests_properties(audio_simulation PROPERTIES TIMEOUT 120)

    add_executable(TelephonyDspRegression tests/TelephonyDspRegression.cpp)
    target_link_libraries(TelephonyDspRegression PRIVATE TelephonyDSP)
    add_test(NAME dsp_regressions COMMAND TelephonyDspRegression)

    add_executable(TelephonyPluginRegression
        tests/TelephonyPluginRegression.cpp
        TelephonyVoice.cpp
        external/vst3sdk/public.sdk/source/common/memorystream.cpp
    )
    target_link_libraries(TelephonyPluginRegression PRIVATE TelephonyDSP sdk)
    target_include_directories(TelephonyPluginRegression PRIVATE external/vst3sdk)
    add_test(NAME plugin_regressions COMMAND TelephonyPluginRegression)

    add_executable(TelephonyRunnerTests tests/TelephonyRunnerTests.cpp)
    target_link_libraries(TelephonyRunnerTests PRIVATE TelephonyDSP)
    add_test(NAME runner_regressions COMMAND TelephonyRunnerTests)
    set_tests_properties(dsp_smoke dsp_regressions plugin_regressions runner_regressions
        PROPERTIES TIMEOUT 120)

    if(NOT TELEPHONY_DISTRIBUTION_BUILD)
        add_executable(TelephonyEvsConfigTests tests/EvsApiConfiguration.c)
        target_link_libraries(TelephonyEvsConfigTests PRIVATE TelephonyDSP)
        if(UNIX)
            target_link_libraries(TelephonyEvsConfigTests PRIVATE m)
        endif()
        add_test(NAME evs_configurations COMMAND TelephonyEvsConfigTests)
        set_tests_properties(evs_configurations PROPERTIES TIMEOUT 120)
    endif()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME editor_description
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ValidateEditor.py)
    endif()

    if(TELEPHONY_USE_EVS_FX AND TELEPHONY_BUILD_EVS_FX_REFERENCE_TOOLS AND Python3_Interpreter_FOUND)
        add_test(NAME evs_fx_api_regressions COMMAND TelephonyEvsFxApiRegression)
        add_test(NAME evs_fx_differential COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/EvsFxDifferential.py
            --encoder $<TARGET_FILE:TelephonyEvsFxReferenceEncoder>
            --decoder $<TARGET_FILE:TelephonyEvsFxReferenceDecoder>
            --adapter $<TARGET_FILE:TelephonyEvsFxReferenceAdapter>)
        set_tests_properties(evs_fx_differential PROPERTIES TIMEOUT 600)
    endif()
    if(TELEPHONY_USE_EVS_JBM)
        add_executable(TelephonyJbmClockDrift tests/JbmClockDrift.cpp)
        target_link_libraries(TelephonyJbmClockDrift PRIVATE TelephonyDSP)
        if(UNIX AND NOT APPLE)
            target_link_options(TelephonyJbmClockDrift PRIVATE -Wl,--wrap=apa_set_scale -Wl,--wrap=apa_exec)
            target_compile_definitions(TelephonyJbmClockDrift PRIVATE TELEPHONY_TEST_APA_WRAP=1)
        endif()
        add_test(NAME jbm_clock_drift COMMAND TelephonyJbmClockDrift)
        set_tests_properties(jbm_clock_drift PROPERTIES TIMEOUT 600)
        add_test(NAME evs_jbm_smoke COMMAND EVSJbmSmoke)
        set_tests_properties(evs_jbm_smoke PROPERTIES TIMEOUT 120)
    endif()
endif()
