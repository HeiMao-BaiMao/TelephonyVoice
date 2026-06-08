set(EVS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/3gpp-evs")

# Mis-named fixed-point source files (no _fx.c suffix, but they pull in
# cnst_fx.h / prot_fx.h and would clash with the float variant).
set(EVS_FX_EXTRA_SOURCES
    "${EVS_ROOT}/lib_com/gain_inov.c"
    "${EVS_ROOT}/lib_enc/vad_basop.c"
)

# Helper: gather sources, excluding the _fx.c variant and CLI-only files
function(evs_collect_float_sources OUT_VAR DIR)
    file(GLOB _srcs "${DIR}/*.c")
    list(FILTER _srcs EXCLUDE REGEX "_fx\\.c$")
    # Drop CLI programs (each contains main() and a global frame counter)
    list(REMOVE_ITEM _srcs
        "${DIR}/encoder.c"
        "${DIR}/decoder.c"
        # CLI argument parsers call exit() on bad input - not safe in a lib
        "${DIR}/io_enc.c"
        "${DIR}/io_dec.c"
        # Mis-named FX files that must not be linked into the float lib
        "${EVS_ROOT}/lib_com/gain_inov.c"
        "${EVS_ROOT}/lib_enc/vad_basop.c"
    )
    set(${OUT_VAR} ${_srcs} PARENT_SCOPE)
endfunction()

# Helper: gather only the _fx.c variant (also dropping CLI parsers) plus
# the mis-named extras declared in EVS_FX_EXTRA_SOURCES.
function(evs_collect_fx_sources OUT_VAR DIR)
    file(GLOB _srcs "${DIR}/*_fx.c")
    list(REMOVE_ITEM _srcs
        "${DIR}/io_enc_fx.c"
        "${DIR}/io_dec_fx.c"
    )
    foreach(_extra ${EVS_FX_EXTRA_SOURCES})
        list(APPEND _srcs ${_extra})
    endforeach()
    set(${OUT_VAR} ${_srcs} PARENT_SCOPE)
endfunction()

# Common include paths for all EVS variants
set(EVS_INCLUDE_DIRS
    "${EVS_ROOT}/lib_com"
    "${EVS_ROOT}/lib_enc"
    "${EVS_ROOT}/lib_dec"
)

# ---------------------------------------------------------------------------
# Floating-point EVS (TS 26.443 v12.7.0/v13.3.0) - default
# ---------------------------------------------------------------------------
evs_collect_float_sources(EVS_LIB_COM_SOURCES "${EVS_ROOT}/lib_com")
add_library(evs-lib-com STATIC ${EVS_LIB_COM_SOURCES})
target_include_directories(evs-lib-com PUBLIC ${EVS_INCLUDE_DIRS})
if(MSVC)
    target_compile_definitions(evs-lib-com PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(evs-lib-com PRIVATE /wd4244 /wd4267 /wd4018 /wd4305)
    # EVS is heavy floating-point code; /O2 keeps it fast and matches upstream
    target_compile_options(evs-lib-com PRIVATE /O2)
    # EVS's basop32.c/basop_util.c implement the same ITU-T G.191 basic
    # arithmetic operations as opencore-amrnb. They are bit-equivalent; pick
    # one at link time.
    target_link_options(evs-lib-com INTERFACE /FORCE:MULTIPLE)
endif()

evs_collect_float_sources(EVS_LIB_ENC_SOURCES "${EVS_ROOT}/lib_enc")
add_library(evs-lib-enc STATIC ${EVS_LIB_ENC_SOURCES})
target_link_libraries(evs-lib-enc PUBLIC evs-lib-com)
target_include_directories(evs-lib-enc PUBLIC ${EVS_INCLUDE_DIRS})
if(MSVC)
    target_compile_definitions(evs-lib-enc PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(evs-lib-enc PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
endif()

evs_collect_float_sources(EVS_LIB_DEC_SOURCES "${EVS_ROOT}/lib_dec")
add_library(evs-lib-dec STATIC ${EVS_LIB_DEC_SOURCES})
target_link_libraries(evs-lib-dec PUBLIC evs-lib-com)
target_include_directories(evs-lib-dec PUBLIC ${EVS_INCLUDE_DIRS})
if(MSVC)
    target_compile_definitions(evs-lib-dec PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(evs-lib-dec PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
endif()

# ---------------------------------------------------------------------------
# Fixed-point EVS (TS 26.442 v16.4.0) - built only when TELEPHONY_USE_EVS_FX=ON
# ---------------------------------------------------------------------------
if(TELEPHONY_USE_EVS_FX)
    evs_collect_fx_sources(EVS_LIB_COM_FX_SOURCES "${EVS_ROOT}/lib_com")
    add_library(evs-lib-com-fx STATIC ${EVS_LIB_COM_FX_SOURCES})
    target_include_directories(evs-lib-com-fx PUBLIC ${EVS_INCLUDE_DIRS})
    if(MSVC)
        target_compile_definitions(evs-lib-com-fx PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(evs-lib-com-fx PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
    endif()

    evs_collect_fx_sources(EVS_LIB_ENC_FX_SOURCES "${EVS_ROOT}/lib_enc")
    add_library(evs-lib-enc-fx STATIC ${EVS_LIB_ENC_FX_SOURCES})
    target_link_libraries(evs-lib-enc-fx PUBLIC evs-lib-com-fx)
    target_include_directories(evs-lib-enc-fx PUBLIC ${EVS_INCLUDE_DIRS})
    if(MSVC)
        target_compile_definitions(evs-lib-enc-fx PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(evs-lib-enc-fx PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
    endif()

    evs_collect_fx_sources(EVS_LIB_DEC_FX_SOURCES "${EVS_ROOT}/lib_dec")
    add_library(evs-lib-dec-fx STATIC ${EVS_LIB_DEC_FX_SOURCES})
    target_link_libraries(evs-lib-dec-fx PUBLIC evs-lib-com-fx)
    target_include_directories(evs-lib-dec-fx PUBLIC ${EVS_INCLUDE_DIRS})
    if(MSVC)
        target_compile_definitions(evs-lib-dec-fx PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(evs-lib-dec-fx PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
    endif()
endif()
