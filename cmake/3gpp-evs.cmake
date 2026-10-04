set(EVS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/3gpp-evs")

# The pinned mixed float/FX snapshot is not a complete fixed-point codec.
# Never fill missing algorithms with the historical link-only helpers.
if(TELEPHONY_USE_EVS_FX)
    include("${CMAKE_CURRENT_LIST_DIR}/3gpp-evs-fx.cmake")
    return()
endif()

# Helper: gather float sources for a directory, excluding the *_fx.c
# variant and the CLI parsers / mis-named FX files.
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

# The vendored float tree also includes a BASOP ROM source that duplicates
# ten tables in rom_com.c. Keep its unique window tables, but isolate the
# duplicate definitions in a generated wrapper. The FX source stays untouched.
set(EVS_FLOAT_BASOP_ROM "${CMAKE_CURRENT_BINARY_DIR}/evs_float_rom_basop.c")
file(WRITE "${EVS_FLOAT_BASOP_ROM}" "/* Generated: isolate duplicate BASOP ROM names. */\n")
foreach(_table ldCoeff exp2_tab_long exp2w_tab_long exp2x_tab_long
        SqrtTable SqrtDiffTable ISqrtTable ISqrtDiffTable InvTable InvDiffTable)
    file(APPEND "${EVS_FLOAT_BASOP_ROM}" "#define ${_table} telephony_basop_${_table}\n")
endforeach()
file(APPEND "${EVS_FLOAT_BASOP_ROM}" "#include \"${EVS_ROOT}/lib_com/rom_basop_util.c\"\n")
list(REMOVE_ITEM EVS_LIB_COM_SOURCES "${EVS_ROOT}/lib_com/rom_basop_util.c")
list(APPEND EVS_LIB_COM_SOURCES "${EVS_FLOAT_BASOP_ROM}")

add_library(evs-lib-com STATIC ${EVS_LIB_COM_SOURCES})
target_include_directories(evs-lib-com PUBLIC ${EVS_INCLUDE_DIRS})
# Rename wb_vad / wb_vad_init to evs_wb_vad* (see header comment above)
target_compile_definitions(evs-lib-com PRIVATE ${EVS_WB_VAD_RENAME_DEFS})
if(MSVC)
    target_compile_definitions(evs-lib-com PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(evs-lib-com PRIVATE /wd4244 /wd4267 /wd4018 /wd4305)
    # EVS is heavy floating-point code; /O2 keeps it fast and matches upstream
    target_compile_options(evs-lib-com PRIVATE /O2)
    # Codec symbols are isolated in cmake/opencore-amr.cmake. Do not suppress
    # duplicate-symbol errors: the apparent duplicates can have different ABIs.
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
    # Work around an MSVC internal compiler error (C1001) in avq_dec.c
    # when compiled with /O2 in this toolchain version.  /Od for this
    # single file avoids the crash without affecting the rest of the
    # decoder library.
    set_source_files_properties("${EVS_ROOT}/lib_dec/avq_dec.c"
        PROPERTIES COMPILE_FLAGS "/Od")
endif()
