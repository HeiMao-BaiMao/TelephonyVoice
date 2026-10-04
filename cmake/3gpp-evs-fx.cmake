# Complete official TS 26.442 reference only. Source code remains outside this
# repository: this option does not download code or grant redistribution rights.
set(TELEPHONY_EVS_FX_SOURCE_DIR "" CACHE PATH
    "Path to the complete official TS 26.442 v16.4.0 c-code directory")
option(TELEPHONY_BUILD_EVS_FX_REFERENCE_TOOLS
    "Build official EVS fixed-point CLI tools for local differential testing" OFF)

if(NOT TELEPHONY_EVS_FX_SOURCE_DIR)
    message(FATAL_ERROR
        "TELEPHONY_USE_EVS_FX requires the complete official TS 26.442 v16.4.0 "
        "source. The pinned external/3gpp-evs tree is incomplete and its legacy "
        "link helpers are not a codec. Set TELEPHONY_EVS_FX_SOURCE_DIR to the "
        "extracted c-code directory from ts_126442v160400p0.zip. See "
        "docs/EVS_FIXED_POINT.md for provenance, usage conditions and testing.")
endif()
get_filename_component(EVS_FX_ROOT "${TELEPHONY_EVS_FX_SOURCE_DIR}" REALPATH)
foreach(_required readme.txt lib_com/prot_fx.h lib_com/stat_com.h
        lib_com/bitstream_fx.c lib_enc/evs_enc_fx.c lib_enc/enc_acelp_tcx_main.c
        lib_enc/pitch_ol.c lib_dec/evs_dec_fx.c basic_op/basop32.c)
    if(NOT EXISTS "${EVS_FX_ROOT}/${_required}")
        message(FATAL_ERROR "Incomplete official EVS FX source: missing ${_required}")
    endif()
endforeach()
file(READ "${EVS_FX_ROOT}/readme.txt" _evs_fx_readme LIMIT 1024)
file(READ "${EVS_FX_ROOT}/lib_com/stat_com.h" _evs_fx_stat_header LIMIT 1024)
if(NOT _evs_fx_readme MATCHES "TS26\\.442[^\n]*16\\.4\\.0" OR
   NOT _evs_fx_stat_header MATCHES "TS26\\.442[^\n]*16\\.4\\.0")
    message(FATAL_ERROR
        "EVS FX needs the unmixed TS 26.442 v16.4.0 reference headers and source. "
        "The mixed TS 26.443 snapshot and other unverified versions are rejected.")
endif()

set(EVS_FX_INCLUDE_DIRS "${EVS_FX_ROOT}/lib_com" "${EVS_FX_ROOT}/lib_enc"
    "${EVS_FX_ROOT}/lib_dec" "${EVS_FX_ROOT}/basic_op" "${EVS_FX_ROOT}/basic_math")

# Source-local symbol isolation. The complete reference needs no
# replacement structures, suppressed headers, float wrappers or BASOP shims.
set(EVS_FX_NAMESPACE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/evs_fx_names.h")
string(CONCAT _evs_fx_namespace
    "/* Generated EVS symbol isolation, not a type/algorithm replacement. */\n"
    "#define wb_vad telephony_evsfx_wb_vad\n"
    "#define wb_vad_init telephony_evsfx_wb_vad_init\n")

# These unprefixed BASOP helpers also occur in opencore/vo-amrwbenc. They are
# not interchangeable ABIs. Preserve link errors for any unaccounted collision.
foreach(_symbol Deemph2 Div_32 Dot_product12 Isqrt L_Comp L_Extract Random Scale_sig)
    string(APPEND _evs_fx_namespace "#define ${_symbol} telephony_evsfx_${_symbol}\n")
endforeach()

file(CONFIGURE OUTPUT "${EVS_FX_NAMESPACE_HEADER}" CONTENT "${_evs_fx_namespace}" @ONLY)

function(evs_configure_official_fx_target target)
    target_include_directories(${target} PUBLIC ${EVS_FX_INCLUDE_DIRS})
    if(MSVC)
        target_compile_options(${target} PRIVATE "/FI${EVS_FX_NAMESPACE_HEADER}" /O2)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE "-include" "${EVS_FX_NAMESPACE_HEADER}")
        target_link_libraries(${target} PUBLIC m)
    endif()
endfunction()

# Keep generic reference header names and symbol renames local to the C adapter;
# never force them into unrelated TelephonyDSP C++ or AMR translation units.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/evs_api_fx.c" PROPERTIES
    INCLUDE_DIRECTORIES "${EVS_FX_INCLUDE_DIRS}")
if(MSVC)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/evs_api_fx.c" PROPERTY
        COMPILE_OPTIONS "/FI${EVS_FX_NAMESPACE_HEADER}")
else()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/evs_api_fx.c" PROPERTY
        COMPILE_OPTIONS "-include;${EVS_FX_NAMESPACE_HEADER}")
endif()

# Several genuine fixed-point source names have no _fx suffix. Collect the
# complete standalone reference tree, not a suffix-selected mixed snapshot.
file(GLOB _evs_fx_common CONFIGURE_DEPENDS "${EVS_FX_ROOT}/lib_com/*.c"
    "${EVS_FX_ROOT}/basic_op/*.c" "${EVS_FX_ROOT}/basic_math/*.c")
file(GLOB _evs_fx_encoder CONFIGURE_DEPENDS "${EVS_FX_ROOT}/lib_enc/*.c")
file(GLOB _evs_fx_decoder CONFIGURE_DEPENDS "${EVS_FX_ROOT}/lib_dec/*.c")
list(REMOVE_ITEM _evs_fx_encoder "${EVS_FX_ROOT}/lib_enc/encoder.c"
    "${EVS_FX_ROOT}/lib_enc/io_enc_fx.c")
list(REMOVE_ITEM _evs_fx_decoder "${EVS_FX_ROOT}/lib_dec/decoder.c"
    "${EVS_FX_ROOT}/lib_dec/io_dec_fx.c")
add_library(evs-lib-com-fx STATIC ${_evs_fx_common})
add_library(evs-lib-enc-fx STATIC ${_evs_fx_encoder})
add_library(evs-lib-dec-fx STATIC ${_evs_fx_decoder})
foreach(_target evs-lib-com-fx evs-lib-enc-fx evs-lib-dec-fx)
    evs_configure_official_fx_target(${_target})
endforeach()
target_link_libraries(evs-lib-enc-fx PUBLIC evs-lib-com-fx)
target_link_libraries(evs-lib-dec-fx PUBLIC evs-lib-com-fx)

if(TELEPHONY_BUILD_EVS_FX_REFERENCE_TOOLS)
    add_executable(TelephonyEvsFxReferenceAdapter
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/EvsFxReferenceAdapter.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/evs_api_fx.c")
    target_include_directories(TelephonyEvsFxReferenceAdapter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(TelephonyEvsFxReferenceAdapter PRIVATE evs-lib-enc-fx evs-lib-dec-fx)
    evs_configure_official_fx_target(TelephonyEvsFxReferenceAdapter)

    add_executable(TelephonyEvsFxReferenceEncoder
        "${EVS_FX_ROOT}/lib_enc/encoder.c" "${EVS_FX_ROOT}/lib_enc/io_enc_fx.c")
    target_link_libraries(TelephonyEvsFxReferenceEncoder PRIVATE evs-lib-enc-fx)
    evs_configure_official_fx_target(TelephonyEvsFxReferenceEncoder)
    add_executable(TelephonyEvsFxReferenceDecoder
        "${EVS_FX_ROOT}/lib_dec/decoder.c" "${EVS_FX_ROOT}/lib_dec/io_dec_fx.c")
    target_link_libraries(TelephonyEvsFxReferenceDecoder PRIVATE evs-lib-dec-fx)
    evs_configure_official_fx_target(TelephonyEvsFxReferenceDecoder)
endif()

add_executable(TelephonyEvsFxApiRegression
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/EvsFxApiRegression.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/evs_api_fx.c")
target_include_directories(TelephonyEvsFxApiRegression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(TelephonyEvsFxApiRegression PRIVATE evs-lib-enc-fx evs-lib-dec-fx)
