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
# EVS `wb_vad` / `wb_vad_init` symbol collision with vo-amrwbenc
#
# Both `external/3gpp-evs` and `external/vo-amrwbenc` export C symbols
# `wb_vad` and `wb_vad_init`. Under MSVC's `INTERFACE /FORCE:MULTIPLE` the
# linker picks one implementation at link time, which corrupts encoder
# state at runtime (the wrong VAD runs for the wrong codec).
#
# The submodule is read-only, so we cannot rename the EVS symbols in source.
# Instead, apply object-like macro renames *at compile time* for every
# translation unit that includes `lib_com/prot.h` (which is every .c file in
# `lib_com`, `lib_enc`, and `lib_dec`). The C preprocessor rewrites every
# whole-word occurrence of `wb_vad` / `wb_vad_init` to its `evs_wb_vad*`
# counterpart during translation, so declarations in `prot.h`, the
# definitions in `lib_enc/vad.c`, and the call sites in
# `lib_enc/{amr_wb_enc,init_enc,pre_proc}.c` all line up.
#
# Comments containing those identifiers are not affected (comments are
# tokenised as comments before preprocessing), so headers and source
# documentation still read `wb_vad` to humans.
# ---------------------------------------------------------------------------
set(EVS_WB_VAD_RENAME_DEFS
    "wb_vad=evs_wb_vad"
    "wb_vad_init=evs_wb_vad_init"
)

# ---------------------------------------------------------------------------
# Floating-point EVS (TS 26.443 v12.7.0/v13.3.0) - default
# ---------------------------------------------------------------------------
evs_collect_float_sources(EVS_LIB_COM_SOURCES "${EVS_ROOT}/lib_com")
add_library(evs-lib-com STATIC ${EVS_LIB_COM_SOURCES})
target_include_directories(evs-lib-com PUBLIC ${EVS_INCLUDE_DIRS})
# Rename wb_vad / wb_vad_init to evs_wb_vad* (see header comment above)
target_compile_definitions(evs-lib-com PRIVATE ${EVS_WB_VAD_RENAME_DEFS})
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
# Rename wb_vad / wb_vad_init to evs_wb_vad* (see header comment above)
target_compile_definitions(evs-lib-enc PRIVATE ${EVS_WB_VAD_RENAME_DEFS})
if(MSVC)
    target_compile_definitions(evs-lib-enc PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(evs-lib-enc PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
endif()

evs_collect_float_sources(EVS_LIB_DEC_SOURCES "${EVS_ROOT}/lib_dec")
add_library(evs-lib-dec STATIC ${EVS_LIB_DEC_SOURCES})
target_link_libraries(evs-lib-dec PUBLIC evs-lib-com)
target_include_directories(evs-lib-dec PUBLIC ${EVS_INCLUDE_DIRS})
# Rename wb_vad / wb_vad_init to evs_wb_vad* (see header comment above)
target_compile_definitions(evs-lib-dec PRIVATE ${EVS_WB_VAD_RENAME_DEFS})
if(MSVC)
    target_compile_definitions(evs-lib-dec PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(evs-lib-dec PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
endif()

# ---------------------------------------------------------------------------
# Fixed-point EVS (TS 26.442 v16.4.0) - built only when TELEPHONY_USE_EVS_FX=ON
# ---------------------------------------------------------------------------
if(TELEPHONY_USE_EVS_FX)
    # Suppress float cnst.h (conflicts with cnst_fx.h).
    set(EVS_FX_FLOAT_SUPPRESS_DEFS CNST_H)

    evs_collect_fx_sources(EVS_LIB_COM_FX_SOURCES "${EVS_ROOT}/lib_com")
    add_library(evs-lib-com-fx STATIC ${EVS_LIB_COM_FX_SOURCES})
    target_include_directories(evs-lib-com-fx PUBLIC ${EVS_INCLUDE_DIRS})
    target_compile_definitions(evs-lib-com-fx PRIVATE ${EVS_FX_FLOAT_SUPPRESS_DEFS})
    target_compile_definitions(evs-lib-com-fx PRIVATE ${EVS_WB_VAD_RENAME_DEFS})
    if(MSVC)
        target_compile_definitions(evs-lib-com-fx PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(evs-lib-com-fx PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
    endif()

    evs_collect_fx_sources(EVS_LIB_ENC_FX_SOURCES "${EVS_ROOT}/lib_enc")
    add_library(evs-lib-enc-fx STATIC ${EVS_LIB_ENC_FX_SOURCES})
    target_link_libraries(evs-lib-enc-fx PUBLIC evs-lib-com-fx)
    target_include_directories(evs-lib-enc-fx PUBLIC ${EVS_INCLUDE_DIRS})
    target_compile_definitions(evs-lib-enc-fx PRIVATE ${EVS_FX_FLOAT_SUPPRESS_DEFS})
    target_compile_definitions(evs-lib-enc-fx PRIVATE ${EVS_WB_VAD_RENAME_DEFS})
    if(MSVC)
        target_compile_definitions(evs-lib-enc-fx PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(evs-lib-enc-fx PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
    endif()

    evs_collect_fx_sources(EVS_LIB_DEC_FX_SOURCES "${EVS_ROOT}/lib_dec")
    add_library(evs-lib-dec-fx STATIC ${EVS_LIB_DEC_FX_SOURCES})
    target_link_libraries(evs-lib-dec-fx PUBLIC evs-lib-com-fx)
    target_include_directories(evs-lib-dec-fx PUBLIC ${EVS_INCLUDE_DIRS})
    target_compile_definitions(evs-lib-dec-fx PRIVATE ${EVS_FX_FLOAT_SUPPRESS_DEFS})
    target_compile_definitions(evs-lib-dec-fx PRIVATE ${EVS_WB_VAD_RENAME_DEFS})
    if(MSVC)
        target_compile_definitions(evs-lib-dec-fx PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(evs-lib-dec-fx PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
    endif()
endif()
