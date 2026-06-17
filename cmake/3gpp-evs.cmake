set(EVS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/3gpp-evs")

# Mis-named fixed-point source files.  These do not follow the *_fx.c
# naming convention but are FX-only (they include cnst_fx.h / prot_fx.h
# and would clash with the float variant if linked into evs-lib-com /
# evs-lib-enc / evs-lib-dec).  Each file is listed alongside the
# directory it lives in so the FX source collector can attach it to
# the correct target only.
set(EVS_FX_EXTRAS_LIB_COM
    # gain_inov.c lives under lib_com but is an FX implementation.
    "${EVS_ROOT}/lib_com/gain_inov.c"
    # get_gain_fx.c is a parent-repo helper for the FX prototype
    # `Word32 get_gain(Word16 x[], Word16 y[], Word16 n)`.  The
    # float counterpart (external/3gpp-evs/lib_com/get_gain.c) has the
    # incompatible signature `float get_gain(float[], float[], int, float*)`
    # and cannot be compiled into the FX lib (it pulls in the float
    # prot.h / options.h chain), so we provide a parent-side C
    # implementation in get_gain_fx.c and attach it here.  The FX
    # call sites in lib_com/cb_shape_fx.c:110 and
    # lib_dec/FEC_scale_syn_fx.c:221,363 are tilt estimators that
    # always use the result in Q16 (L_shr(.., 1) and L_shl(.., 15) in
    # the call sites) so a Q16 Word32 return is the right contract.
    # Uses the same FX include wiring (basop, shim) as the rest of
    # evs-lib-com-fx because it is added to that target's source list.
    "${CMAKE_CURRENT_SOURCE_DIR}/get_gain_fx.c"
    # lerp_fx.c is a parent-repo helper for the FX prototype
    # `void lerp(Word16 *f, Word16 *f_out, Word16 bufferNewSize,
    # Word16 bufferOldSize)`.  The float counterpart
    # (external/3gpp-evs/lib_com/lerp.c) has the incompatible
    # signature `void lerp(float[], float[], int, int)` and pulls in
    # <math.h>, <stdlib.h>, and the float prot.h, so it cannot be
    # compiled into the FX lib.  There is no upstream `lerp_fx.c` -
    # the FX tree relies on the float symbol being callable from
    # FX, which it is not.  Parent-side fixed-point port lives in
    # lerp_fx.c and is attached here.  FX call sites include
    # lib_com/syn_filt_fx.c:251,255,264, lib_enc/core_enc_init.c:370,
    # 386,402,512,516,612, lib_dec/acelp_core_dec_fx.c:217,218,
    # lib_dec/amr_wb_dec_fx.c:233,234, lib_dec/core_switching_dec_fx.c:
    # 420,453,713,717, lib_dec/FEC_clas_estim_fx.c:127, and
    # lib_dec/er_dec_*.  Uses the same FX include wiring (basop,
    # shim) as the rest of evs-lib-com-fx because it is added to
    # that target's source list.
    "${CMAKE_CURRENT_SOURCE_DIR}/lerp_fx.c"
    # basop1616_fx.c is a parent-repo helper that provides four small
    # fixed-point arithmetic primitives - `idiv1616`, `imult1616`,
    # `divide1616`, `divide3232` - that are *called* by vendored 3GPP
    # EVS FX sources but have no implementation (and no prototype) in
    # external/3gpp-evs/.  Upstream equivalent definitions exist only
    # in the float reference tree (basop_util.c ships the unsigned
    # variant `idiv1616U`); the signed versions used here are missing
    # from this vendored snapshot.  FX call sites include
    # lib_com/modif_fs_fx.c:131, lib_com/lpc_tools_fx.c:625,649,713,725,
    # lib_com/bitstream_fx.c:960, lib_com/est_tilt_fx.c:240,
    # lib_enc/pre_proc_fx.c:636,796, lib_dec/core_switching_dec_fx.c:402,
    # and lib_dec/evs_dec_fx.c:550,565,580.  Plain C99 with
    # `long long` intermediates so we do not depend on any other
    # basop helper.  Uses the same FX include wiring as the rest of
    # evs-lib-com-fx (the typedef shim provides Word16/Word32/MAX_16
    # /MIN_16 via /FI).
    "${CMAKE_CURRENT_SOURCE_DIR}/basop1616_fx.c"
    # basop_extra_fx.c is a parent-repo helper that provides three
    # additional BASOP utility primitives - `getScaleFactor16`,
    # `getSqrtWord32`, `getNormReciprocalWord16` - that are called by
    # vendored 3GPP EVS FX sources but have no implementation in the
    # vendored snapshot.  FX call sites include
    # lib_com/lpc_tools_fx.c:835, lib_dec/evs_dec_fx.c:961,964
    # (getScaleFactor16), lib_com/index_pvq_opt_fx.c:672 and
    # lib_dec/pvq_core_dec_fx.c:398,406,435,466 (getSqrtWord32), and
    # lib_com/window_ola_fx.c:612,664 (getNormReciprocalWord16).
    # Plain C99 with 64-bit intermediates; only typedefs.h is included,
    # matching the other parent helpers.
    "${CMAKE_CURRENT_SOURCE_DIR}/basop_extra_fx.c"
    # tns_base_fx.c is a parent-repo helper that provides the fixed-point
    # TNS accessor family declared in external/3gpp-evs/lib_com/prot_fx.h
    # (lines 9747-9778).  The upstream float implementation in
    # external/3gpp-evs/lib_com/tns_base.c uses the float Decoder_State and
    # int-based signatures, so it cannot be linked into the FX library.
    # Uses the FX include wiring (basop, shim) and the FX TNS tables in
    # external/3gpp-evs/lib_com/rom_com_fx.h / rom_com_fx.c.
    "${CMAKE_CURRENT_SOURCE_DIR}/tns_base_fx.c"
    # cldfb_fx.c is a parent-repo helper that provides the fixed-point
    # CLDFB accessor family declared in external/3gpp-evs/lib_com/prot_fx.h
    # (cldfbAnalysisFiltering, cldfbSynthesisFiltering, openCldfb,
    # deleteCldfb, resampleCldfb, cldfb_save/restore/reset_memory and
    # CLDFB_getNumChannels).  The upstream float implementation in
    # external/3gpp-evs/lib_com/cldfb.c cannot be linked into the FX
    # library, and the vendored fixed-point snapshot does not ship the
    # corresponding FX implementation.  Uses the FX include wiring
    # (basop, shim) and the FX CLDFB prototype-filter ROM tables in
    # rom_com_fx.h / rom_com_fx.c.
    "${CMAKE_CURRENT_SOURCE_DIR}/cldfb_fx.c"
    # tec_tfa_tbe_fx.c is a parent-repo helper that provides the seven
    # TEC/TFA TBE FX symbols declared in external/3gpp-evs/lib_com/prot_fx.h
    # (tfaCalcEnv_fx, tfaEnc_TBE_fx, tecEnc_TBE_fx, set_TEC_TFA_code_fx,
    # procTecTfa_TBE_Fx, calcGainTemp_TBE_Fx, calcLoEnvCheckCorrHiLo_Fix).
    # The upstream float implementations in external/3gpp-evs/lib_com/tec_com.c
    # and external/3gpp-evs/lib_enc/tfa_enc.c cannot be linked into the FX
    # library, and the vendored fixed-point snapshot does not ship the
    # corresponding FX implementation.  Uses the FX include wiring (basop,
    # shim) and the FX TEC/TFA ROM tables in rom_com_fx.h / rom_com_fx.c.
    "${CMAKE_CURRENT_SOURCE_DIR}/tec_tfa_tbe_fx.c"
    # basic_utils_fx.c is a parent-repo helper that provides five foundational
    # FX symbols declared in external/3gpp-evs/lib_com/prot_fx.h:
    # hp20, lag_wind, adapt_lag_wind, fft16, and BASOP_cfft.  The upstream
    # float implementations (hp50.c, lag_wind.c, fft.c) have incompatible
    # float-based ABIs, and the vendored fixed-point snapshot does not ship
    # the FX variants.  hp20 and the lag-window functions are implemented in
    # plain fixed-point; fft16 and BASOP_cfft wrap the existing upstream FX
    # FFT core DoRTFTn_fx() for power-of-two sizes, with a generic complex-DFT
    # fallback for other sizes so the link can proceed to expose the next
    # layer of unresolved symbols.
    "${CMAKE_CURRENT_SOURCE_DIR}/basic_utils_fx.c"
    # pitch_fx.c is a parent-repo helper that provides the eight pitch-related
    # FX symbols declared in external/3gpp-evs/lib_com/prot_fx.h:
    # pitch_ol_init_fx, pitch_ol_fx, pit_decode_fx, pit_Q_dec_fx,
    # pit16k_Q_dec_fx, abs_pit_dec_fx, delta_pit_dec_fx, and
    # pitch_pred_linear_fit.  The upstream float implementations in
    # lib_enc/pitch_ol.c, lib_dec/pit_dec.c, and lib_dec/pitch_extr.c have
    # incompatible float ABIs, and the vendored fixed-point snapshot does not
    # ship the FX variants.  The open-loop pitch search and the FEC linear-fit
    # extrapolation are currently link-unblock stubs; the pitch decoders are
    # direct fixed-point ports of the float reference.
    "${CMAKE_CURRENT_SOURCE_DIR}/pitch_fx.c"
    # fdcng_enc_fx.c is a parent-repo helper that provides the encoder-side
    # FD-CNG symbols declared in external/3gpp-evs/lib_com/prot_fx.h:
    # createFdCngEnc, deleteFdCngEnc, initFdCngEnc, configureFdCngEnc,
    # resetFdCngEnc, perform_noise_estimation_enc, FdCng_exc,
    # FdCng_encodeSID, generate_comfort_noise_enc, and noisy_speech_detection.
    # The upstream float implementations in lib_enc/fd_cng_enc.c and
    # lib_com/fd_cng_com.c are not compiled into the FX static libraries.
    # Full STFT/MSVQ encoder paths are stubbed where the FX typedef shim's
    # FD_CNG_COM layout does not carry the required float-only fields.
    "${CMAKE_CURRENT_SOURCE_DIR}/fdcng_enc_fx.c"
    # fdcng_dec_fx.c is a parent-repo helper that provides the decoder-side
    # FD-CNG symbols declared in external/3gpp-evs/lib_com/prot_fx.h:
    # createFdCngDec, initFdCngDec, deleteFdCngDec, configureFdCngDec,
    # ApplyFdCng, FdCng_decodeSID, generate_comfort_noise_dec,
    # generate_comfort_noise_dec_hf, generate_masking_noise, and
    # noisy_speech_detection.  The upstream float implementations in
    # lib_dec/fd_cng_dec.c and lib_com/fd_cng_com.c are not compiled into the
    # FX static libraries.  Comfort-noise / masking-noise synthesis are
    # link-unblock stubs for now.
    "${CMAKE_CURRENT_SOURCE_DIR}/fdcng_dec_fx.c"
    # acelp_core_fx.c is a parent-repo helper that provides the ACELP core
    # fixed-point symbols declared in external/3gpp-evs/lib_com/prot_fx.h:
    # E_ACELP_codebook_corr, E_ACELP_codebook_target_update,
    # E_ACELP_convolve, E_ACELP_correlation, E_ACELP_innovative_codeword,
    # E_ACELP_q_pulse, E_ACELP_xAq, E_ACELP_xh_corr, E_ACELP_1 algebraic
    # codebook search, encode_acelp_gains, E_GAIN_closed_loop_search,
    # BITS_ALLOC_config_acelp, and Unified_weighting_fx.  The upstream float
    # implementations in lib_enc/acelp_enc.c, lib_enc/g_acelp_enc.c and
    # lib_enc/enc_lag.c have incompatible float ABIs, and the vendored
    # fixed-point snapshot does not ship the FX variants.  The algebraic
    # codebook / gain-quantisation / closed-loop-search paths are currently
    # link-unblock stubs.
    "${CMAKE_CURRENT_SOURCE_DIR}/acelp_core_fx.c"
    # core_enc_vad_fx.c is a parent-repo helper that provides the encoder-side
    # core / VAD / preprocessing symbols declared in
    # external/3gpp-evs/lib_com/prot_fx.h: enc_acelp_tcx_main,
    # core_encode_update, init_coder_ace_plus, MDCT_selector_reset,
    # InitTransientDetection, enc_prm_rf, SetModeIndex,
    # analysisCldfbEncoder_fx, MDCT_selector, long_enr_fx, find_uv_fx,
    # signal_clas_fx, core_acelp_tcx20_switching, analy_sp, AdjustFirstSID,
    # RunTransientDetection, GetTCXAvgTemporalFlatnessMeasure, SetTCXModeInfo,
    # and vad_proc.  The upstream float implementations live in
    # lib_enc/{enc_acelp_tcx_main,core_enc_updt,core_enc_init,mdct_selector,
    # transient_detection,enc_lag}.c and lib_com/cldfb.c.  They use float
    # Encoder_State / float arrays and cannot be linked into the FX static
    # libraries.  Full ACELP/TCX core encoding and RF parameter packing are
    # currently link-unblock stubs.
    "${CMAKE_CURRENT_SOURCE_DIR}/core_enc_vad_fx.c"
    # dec_postfilter_fx.c is a parent-repo helper that provides the decoder-side
    # post-filter / concealment fixed-point symbols declared in
    # external/3gpp-evs/lib_com/prot_fx.h: init_decoder_LPD_fx,
    # open_decoder_LPD, close_decoder_LPD, decode_gn_lpc, speech_music_class,
    # acelp_mode_dec, core_decoder_signal, tcx_ltp_post, lpd_delay_switch,
    # decoder_LPD_status, resynch_LPD, lpd_get_closest_freq_arry,
    # lpd_get_closest_pitch_arry, and scale_st.  The upstream float
    # implementations live in lib_dec/{dec_acelp,dec_lpd,lpd_dec,
    # core_dec_signal}.c and lib_com/tcx_ltp.c.  They use float Decoder_State /
    # float arrays and cannot be linked into the FX static libraries.  Full
    # LPD decoder state-machine paths are currently link-unblock stubs.
    "${CMAKE_CURRENT_SOURCE_DIR}/dec_postfilter_fx.c"
)
set(EVS_FX_EXTRAS_LIB_ENC
    # vad_basop.c lives under lib_enc but is an FX implementation.
    "${EVS_ROOT}/lib_enc/vad_basop.c"
)

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

# Helper: gather fixed-point sources for a directory.  The glob picks
# up only the *_fx.c variant; the caller can append directory-specific
# mis-named extras via the EXTRAS argument so that, for example,
# lib_com/gain_inov.c is only attached to evs-lib-com-fx and not to
# evs-lib-enc-fx / evs-lib-dec-fx.
function(evs_collect_fx_sources OUT_VAR DIR)
    file(GLOB _srcs "${DIR}/*_fx.c")
    list(REMOVE_ITEM _srcs
        "${DIR}/io_enc_fx.c"
        "${DIR}/io_dec_fx.c"
    )
    if(ARGN)
        foreach(_extra ${ARGN})
            list(APPEND _srcs ${_extra})
        endforeach()
    endif()
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
#
# The 3GPP EVS reference keeps one set of headers per directory and
# guards them with directory-local include sentinels (e.g. TYPEDEF_H,
# _TYPEDEF_H, STAT_COM_H, CNST_H).  When the FX variant is built, the
# *float* lib_com headers (lib_com/typedef.h, lib_com/cnst.h, ...)
# would otherwise pull in the *float* Word16/Word32/bool/CNX constants
# and clash with the *fixed-point* Word16/Word32/bool/CNX constants
# defined in basic_op/typedefs.h, basic_op/basop32.h, and cnst_fx.h.
#
# The submodule is read-only, so we cannot edit the offending headers.
# Instead we inject a parent-repo-only shim:
#
#   1. A generated header ${EVS_FX_TYPEDEF_SHIM} (see below) is
#      force-included into every FX translation unit.  It:
#        a) includes basic_op/typedefs.h (the new ITU-T G.191 type
#           definitions) and provides the missing `Flag` and `Float32`
#           typedefs,
#        b) pre-defines TYPEDEF_H and _TYPEDEF_H so that any later
#           `#include "typedef.h"` (from lib_com/typedef.h,
#           basic_op/typedef.h, or basop_util.h) becomes a no-op,
#        c) includes lib_com/cnst_fx.h (the fixed-point constant
#           table) and fills in the few constants it expects but
#           does not define,
#        d) pre-defines CNST_H so that any later `#include "cnst.h"`
#           (from lib_com/stat_com.h, prot.h, ...) is a no-op, and
#        e) includes lib_com/stat_com.h so the cross-module structures
#           (PFSTAT, IGF_INFO, FD_CNG_COM, CLDFB_FILTER_BANK, TEC_DEC,
#           TEC_ENC, TCX_config, ...) are visible *before* any FX
#           translation unit sees stat_dec_fx.h / stat_enc_fx.h via
#           lib_com/prot_fx.h.  This sidesteps the ordering problem
#           where prot_fx.h includes stat_dec_fx.h / stat_enc_fx.h
#           before stat_com.h: by the time prot_fx.h runs, stat_com.h
#           has already been processed by the shim, so its guard
#           STAT_COM_H is set and its #include "cnst.h" /
#           #include "typedef.h" lines become no-ops (thanks to the
#           CNST_H and TYPEDEF_H predefines), and the shared struct
#           definitions are visible to the FX headers.
#
#   2. STAT_COM_H is intentionally NOT pre-defined manually; letting
#      stat_com.h set its own guard keeps the contract simple.  There
#      is no `stat_com_fx.h` companion: stat_com.h is a *shared*
#      header that defines the cross-module structures used by
#      stat_dec_fx.h, stat_enc_fx.h, and rom_com_fx.h.
# ---------------------------------------------------------------------------
if(TELEPHONY_USE_EVS_FX)
    # ------------------------------------------------------------------
    # Generated FX typedef shim
    # ------------------------------------------------------------------
    set(EVS_FX_SHIM_DIR "${CMAKE_CURRENT_BINARY_DIR}/evs_fx_shim")
    set(EVS_FX_TYPEDEF_SHIM "${EVS_FX_SHIM_DIR}/evs_fx_typedef_shim.h")
    file(MAKE_DIRECTORY "${EVS_FX_SHIM_DIR}")
    file(WRITE "${EVS_FX_TYPEDEF_SHIM}"
        "/* Auto-generated by cmake/3gpp-evs.cmake. Do not edit. */\n"
        "/*\n"
        " * FX-only forced-include shim for the 3GPP EVS reference.\n"
        " *\n"
        " * Provides the platform-independent basic-operator types (Word16,\n"
        " * Word32, ...) once, then suppresses the float lib_com/typedef.h,\n"
        " * basic_op/typedef.h, and lib_com/cnst.h by pre-defining their\n"
        " * include guards.  The shared lib_com/stat_com.h is *not*\n"
        " * suppressed; instead we include it eagerly so its cross-module\n"
        " * structures (PFSTAT, IGF_INFO, FD_CNG_COM, CLDFB_FILTER_BANK,\n"
        " * TEC_DEC, TEC_ENC, TCX_config, ...) are defined *before* any FX\n"
        " * translation unit reaches lib_com/prot_fx.h (which otherwise\n"
        " * includes stat_dec_fx.h / stat_enc_fx.h before stat_com.h).\n"
        " * When stat_com.h runs here, its own `#include \"cnst.h\"` /\n"
        " * `#include \"typedef.h\"` lines become no-ops thanks to the\n"
        " * CNST_H and TYPEDEF_H predefines below, so nothing float-\n"
        " * specific is dragged back in, and its own guard (STAT_COM_H)\n"
        " * is set naturally.\n"
        " */\n"
        "#ifndef EVS_FX_TYPEDEF_SHIM_H\n"
        "#define EVS_FX_TYPEDEF_SHIM_H\n"
        "\n"
        "/* basic_op/typedefs.h is the new ITU-T G.191 type set\n"
        " * (Char, Bool, Word8/16/32, UWord8/16/32, Word40, Float, CPX, ...).\n"
        " * Force its body to be processed by clearing its guard. */\n"
        "#undef _TYPEDEFS_H\n"
        "#include \"typedefs.h\"\n"
        "\n"
        "/* basic_op/typedefs.h does not define `Flag` or `Float32`; the\n"
        " * float lib_com/typedef.h does, but its body is about to be\n"
        " * skipped (see below).  Provide them here so FX sources that\n"
        " * rely on Flag / Float32 compile.  The `#ifndef` guards make\n"
        " * this safe even if a downstream header also tries to define\n"
        " * them. */\n"
        "#ifndef Flag\n"
        "typedef int Flag;\n"
        "#endif\n"
        "#ifndef Float32\n"
        "typedef float Float32;\n"
        "#endif\n"
        "\n"
        "/* Now that Word16/Word32/... are visible, pre-define the guards\n"
        " * used by the float lib_com/typedef.h (`TYPEDEF_H`) and the old\n"
        " * basic_op/typedef.h (`_TYPEDEF_H`).  Both headers' bodies are\n"
        " * wrapped in `#ifndef ...` so subsequent `#include \"typedef.h\"`\n"
        " * from basop_util.h, prot.h, stat_com.h, etc. become no-ops,\n"
        " * preventing Word16 / UWord16 / etc. redefinition clashes. */\n"
        "#define _TYPEDEF_H\n"
        "#define TYPEDEF_H\n"
        "\n"
        "/* Pull in the basic_op `enh40.h` (slim prototype-only header)\n"
        " * NOW, before any include that could transitively pull in\n"
        " * `lib_com/enh40.h`.  Both files use the shared `_ENH40_H`\n"
        " * include guard, so whichever is included first sets the guard\n"
        " * and the other one becomes a no-op.  We must let the slim\n"
        " * `basic_op/enh40.h` win, not the lib_com one, because\n"
        " * `lib_com/enh40.h` carries `static __inline` definitions of\n"
        " * L40_set / Extract40_L / L40_mult / L40_add / L40_mac /\n"
        " * L_Extract40 (with bodies!) - those bodies are TU-local, so\n"
        " * once the lib_com header has been processed in a translation\n"
        " * unit, that TU already has those static-inline symbols, and\n"
        " * the file-scope `static __inline` re-definitions in\n"
        " * `basic_op/enh40.c` collide with them (MSVC C2084: function\n"
        " * already has a body).  Forcing the slim header first ensures\n"
        " * the only `static __inline` L40_* definitions in the TU are\n"
        " * the ones in `basic_op/enh40.c` itself.\n"
        " *\n"
        " * The FX include path is `basic_op/` before `lib_com/`, and\n"
        " * the shim is force-included from each source file via /FI<shim>\n"
        " * (which makes the C preprocessor treat the shim as if it were\n"
        " * included at the top of the source file).  For sources under\n"
        " * `basic_op/`, source-dir-relative lookup picks\n"
        " * `basic_op/enh40.h`; for sources under `lib_com/`, `lib_enc/`,\n"
        " * `lib_dec/`, or `basic_math/`, the include-path lookup still\n"
        " * finds `basic_op/enh40.h` first (since `basic_op/` comes\n"
        " * before `lib_com/` in EVS_FX_INCLUDE_DIRS).  Either way the\n"
        " * slim header is the one that sets `_ENH40_H`, and the lib_com\n"
        " * variant becomes a no-op whenever it is encountered later\n"
        " * (e.g. via `lib_com/stl.h` from `basop_mpy.h`).\n"
        " *\n"
        " * `basic_op/enh40.h` only declares Mpy_32_16_ss / Mpy_32_32_ss\n"
        " * prototypes plus a recursive `#include \"stl.h\"` (which itself\n"
        " * expands to `basic_op/stl.h`; its include guard `_STL_H` is\n"
        " * not pre-defined, so it runs once, sets `_STL_H`, and pulls in\n"
        " * the rest of the basic_op header chain - basic_op/basop32.h,\n"
        " * count.h, move.h, control.h (safe WMOPS=0 form, see override\n"
        " * below), enh1632.h, enh40.h (no-op now), enhUL32.h, plus the\n"
        " * basic_math fallbacks for oper_32b.h / math_op.h / log2.h\n"
        " * which are all no-ops or safe prototype-only headers).\n"
        " * Subsequent `#include \"stl.h\"` (from the source file itself,\n"
        " * from `lib_com/basop_mpy.h` -> `lib_com/stl.h`, etc.) hits the\n"
        " * `_STL_H` guard and is a no-op, so the `static __inline`\n"
        " * bodies from `basic_op/move.h` and the control-flow macros\n"
        " * from `basic_op/control.h` are not duplicated.\n"
        " *\n"
        " * IMPORTANT: do NOT pre-define `_STL_H` here - the shim's\n"
        " * later `#define _CONTROL_H` is guarded with `#ifndef`, and\n"
        " * `basic_op/control.h` (which sets `_CONTROL_H` to the safe\n"
        " * WMOPS=0 keyword-only form) is now reached via this include.\n"
        " * Pre-defining `_STL_H` would also work, but skipping the\n"
        " * `basic_op/stl.h` body means `basic_op/control.h` would not\n"
        " * be processed and the broken-lib-com control.h form would be\n"
        " * the one we override against, which is more brittle. */\n"
        "#include \"enh40.h\"\n"
        "\n"
        "/* Pull in the FX constant table (lib_com/cnst_fx.h).  This\n"
        " * defines the fixed-point versions of L_FRAME_*, L_SUBFR,\n"
        " * MAX_*, MIN_*, NS2SA, ... which the float lib_com/cnst.h\n"
        " * would otherwise redefine with different (float) values.\n"
        " * The float lib_com/cnst.h is then suppressed by the CNST_H\n"
        " * predefine further down. */\n"
        "#include \"cnst_fx.h\"\n"
        "\n"
        "/* A handful of constants used by the shared lib_com/stat_com.h\n"
        " * (and stat_com.h's downstream consumers) are defined in the\n"
        " * float lib_com/cnst.h but missing from lib_com/cnst_fx.h.\n"
        " * Read-only access to external/3gpp-evs/lib_com/cnst.h: the\n"
        " * values below are copy-pasted from the float counterpart.\n"
        " * `#ifndef` guards make this safe even if a future 3GPP\n"
        " * revision closes the gap.  L_MDCT_HALF_OVLP_MAX depends on\n"
        " * L_MDCT_OVLP_MAX and L_MDCT_TRANS_OVLP_MAX on NS2SA, both of\n"
        " * which are now visible via cnst_fx.h above. */\n"
        "#ifndef ACELP_TCX_TRANS_NS\n"
        "#define ACELP_TCX_TRANS_NS                1250000L\n"
        "#endif\n"
        "#ifndef L_MDCT_HALF_OVLP_MAX\n"
        "#define L_MDCT_HALF_OVLP_MAX              (L_MDCT_OVLP_MAX/2)\n"
        "#endif\n"
        "#ifndef L_MDCT_MIN_OVLP_MAX\n"
        "#define L_MDCT_MIN_OVLP_MAX               60\n"
        "#endif\n"
        "#ifndef L_MDCT_TRANS_OVLP_MAX\n"
        "#define L_MDCT_TRANS_OVLP_MAX             NS2SA(48000, ACELP_TCX_TRANS_NS)\n"
        "#endif\n"
        "\n"
        "/* CLDFB_TYPE is an enum normally defined in the float\n"
        " * lib_com/cnst.h (typedef enum { CLDFB_ANALYSIS, CLDFB_SYNTHESIS }\n"
        " * CLDFB_TYPE;) but cnst.h's body is suppressed above by the\n"
        " * CNST_H predefine.  lib_com/stat_com.h references CLDFB_TYPE in\n"
        " * its CLDFB_FILTER_BANK struct definition (line 530), so the enum\n"
        " * must be visible *before* stat_com.h is included.  C cannot test\n"
        " * type existence with `#ifndef`, so use a private shim macro\n"
        " * guard (`EVS_FX_SHIM_HAS_CLDFB_TYPE`) to keep the typedef unique\n"
        " * if this shim ever gets force-included twice in the same TU.\n"
        " *\n"
        " * NOTE: lib_com/cnst_fx.h (included earlier in this shim) defines\n"
        " * CLDFB_ANALYSIS and CLDFB_SYNTHESIS as `#define` integer macros\n"
        " * (`#define CLDFB_ANALYSIS 0` / `#define CLDFB_SYNTHESIS 1`), so\n"
        " * the enum names below would be token-replaced to `0, 1` and the\n"
        " * compiler would reject them with `error C2059: syntax error:\n"
        " * 'constant'`.  `#undef` them first; cnst_fx.h's macro values\n"
        " * exactly match what the enum would assign anyway (the enumerators\n"
        " * start at 0 by default), so nothing semantic is lost. */\n"
        "#ifndef EVS_FX_SHIM_HAS_CLDFB_TYPE\n"
        "#define EVS_FX_SHIM_HAS_CLDFB_TYPE\n"
        "#undef CLDFB_ANALYSIS\n"
        "#undef CLDFB_SYNTHESIS\n"
        "typedef enum\n"
        "{\n"
        "    CLDFB_ANALYSIS,\n"
        "    CLDFB_SYNTHESIS\n"
        "} CLDFB_TYPE;\n"
        "#endif\n"
        "\n"
        "/* CLDFB_SCALE_FACTOR is referenced as a struct type by FX sources\n"
        " * in lib_com/prot_fx.h (cldfbAnalysisFiltering, cldfbSynthesisFiltering,\n"
        " * ...) and by lib_dec/stat_dec_fx.h (DECODER_STATE_FX.scaleFactor),\n"
        " * but neither lib_com/cnst.h nor lib_com/cnst_fx.h defines it.  Read-\n"
        " * only access to external/3gpp-evs confirms the type is expected to\n"
        " * be supplied by the parent project.  Usage in lib_dec/bass_psfilter_fx.c\n"
        " * (s_max(scale.lb_scale, cldfb_scale->lb_scale)), lib_dec/amr_wb_dec_fx.c\n"
        " * (scaleFactor.hb_scale = scaleFactor.lb_scale), and lib_enc/pre_proc_fx.c\n"
        " * (cldfbScale->hb_scale = cldfbScale->lb_scale) shows two `Word16`\n"
        " * fields `lb_scale` and `hb_scale`.  Same private shim guard pattern\n"
        " * as CLDFB_TYPE. */\n"
        "#ifndef EVS_FX_SHIM_HAS_CLDFB_SCALE_FACTOR\n"
        "#define EVS_FX_SHIM_HAS_CLDFB_SCALE_FACTOR\n"
        "typedef struct\n"
        "{\n"
        "    Word16 lb_scale;\n"
        "    Word16 hb_scale;\n"
        "} CLDFB_SCALE_FACTOR;\n"
        "#endif\n"
        "\n"
        "/* Finally, suppress the float lib_com/cnst.h body.  Its guard\n"
        " * is CNST_H, so any subsequent `#include \"cnst.h\"` (from\n"
        " * lib_com/stat_com.h, prot.h, etc.) is a no-op and the FX\n"
        " * constants supplied above win.  STAT_COM_H is intentionally\n"
        " * left undefined here so lib_com/stat_com.h is still processed\n"
        " * when we include it just below; its own guard will be set\n"
        " * naturally on first inclusion. */\n"
        "#define CNST_H\n"
        "\n"
        "/* Pull in the shared lib_com/stat_com.h now so its cross-module\n"
        " * structures (PFSTAT, IGF_INFO, FD_CNG_COM, CLDFB_FILTER_BANK,\n"
        " * TEC_DEC, TEC_ENC, TCX_config, ...) are defined *before* any FX\n"
        " * translation unit reaches lib_com/prot_fx.h (which includes\n"
        " * stat_dec_fx.h and stat_enc_fx.h before stat_com.h).  Those\n"
        " * stat_*_fx.h headers reference IGF_INFO and other types from\n"
        " * stat_com.h, so the shared header has to be processed first.\n"
        " *\n"
        " * When stat_com.h runs here, its own `#include \"typedef.h\"` and\n"
        " * `#include \"cnst.h\"` lines are no-ops thanks to the TYPEDEF_H\n"
        " * and CNST_H predefines above, so no float Word16 / Word32 / CNX\n"
        " * constants are dragged back in.  Its guard (STAT_COM_H) is set\n"
        " * naturally, so any later `#include \"stat_com.h\"` (from\n"
        " * stat_dec_fx.h, stat_enc_fx.h, prot_fx.h, ...) becomes a no-op\n"
        " * and the structs stay unique. */\n"
        "/* stat_com.h defines CLDFB_FILTER_BANK (and HANDLE_CLDFB_FILTER_BANK)\n"
        " * with float fields. The FX code references a `FilterStates`\n"
        " * `Word16 *` member that the float struct does not have\n"
        " * (`lib_enc/swb_pre_proc_fx.c:423`:\n"
        " *      set16_fx( st_fx->cldfbSyn_Fx->FilterStates, 0,\n"
        " *                st_fx->cldfbSyn_Fx->p_filter_length\n"
        " *              + st_fx->cldfbSyn_Fx->no_channels\n"
        " *              * st_fx->cldfbSyn_Fx->no_col );\n"
        " * ), and additionally expects the integer/scalar fields to be\n"
        " * Word16 not int (e.g. `init_enc_fx.c:941`\n"
        " * `initFdCngEnc(..., st_fx->cldfbAna_Fx->scale)` whose prototype in\n"
        " * prot_fx.h is `void initFdCngEnc(HANDLE_FD_CNG_ENC, Word32,\n"
        " * Word16)`, and `init_dec_fx.c:872` `initFdCngDec(...,\n"
        " * st_fx->cldfbSyn_fx->scale)` where prot_fx.h's prototype is\n"
        " * `Word16 initFdCngDec(HANDLE_FD_CNG_DEC, Word16)`).  Remap the\n"
        " * typedef names to private aliases *before* stat_com.h is\n"
        " * processed so the float struct is defined under those aliases;\n"
        " * after the include we `#undef` the macros and provide an\n"
        " * FX-native CLDFB_FILTER_BANK with Word16 fields plus\n"
        " * FilterStates.  The same remap-pre-define / undef-redefine\n"
        " * pattern is used for the `Mpy_32_16` collision above. */\n"
        "/* stat_com.h also defines FD_CNG_SETUP / FD_CNG_COM /\n"
        " * HANDLE_FD_CNG_COM with float fields and `int` scalars.\n"
        " * The FX counterpart in lib_com/rom_com_fx.c (FdCngSetup_nb,\n"
        " * FdCngSetup_wb1, ..., sidPartitions_*, shapingPartitions_*)\n"
        " * uses Word16 for fftlen / stopFFTbin / numPartitions /\n"
        " * numShapingPartitions and `const Word16*` for the partition\n"
        " * tables; the FX decoder state accesses fields via\n"
        " * st_fx->hFdCngDec_fx->hFdCngCom (e.g. `olapBufferSynth2`,\n"
        " * `timeDomainBuffer`, `A_cng`, `frame_type_previous`,\n"
        " * `flag_noisy_speech`, `likelihood_noisy_speech`, `frameSize`,\n"
        " * `fftlen`, `numCoreBands`, `regularStopBand`, `CngBitrate`,\n"
        " * and `fftlenFac` -- a field the float struct does not have\n"
        " * but lib_dec/acelp_core_dec_fx.c:1047 reads as\n"
        " * `st_fx->hFdCngDec_fx->hFdCngCom->fftlenFac` in a `mult_r`\n"
        " * call).  Apply the same remap-pre-define / undef-redefine\n"
        " * pattern as CLDFB_FILTER_BANK so the float FD_CNG types land\n"
        " * under hidden aliases; after the include we undef the\n"
        " * macros and provide an FX-native FD_CNG_SETUP /\n"
        " * FD_CNG_COM / HANDLE_FD_CNG_COM with Word16 fields, the\n"
        " * missing `fftlenFac`, and the array members sized for the\n"
        " * fixed-point code paths. */\n"
        "#define CLDFB_FILTER_BANK evs_fx_clfb_f\n"
        "#define HANDLE_CLDFB_FILTER_BANK evs_fx_h_clfb_f\n"
        "#define FD_CNG_SETUP evs_fx_fdcngsetup_f\n"
        "#define FD_CNG_COM evs_fx_fdcngcom_f\n"
        "#define HANDLE_FD_CNG_COM evs_fx_h_fdcngcom_f\n"
        "#include \"stat_com.h\"\n"
        "\n"
        "/* Undefine the remap macros now that stat_com.h has been\n"
        " * processed. The float CLDFB_FILTER_BANK is still defined in the\n"
        " * TU under the alias `evs_fx_clfb_f` (which nothing else\n"
        " * references), so we can safely shadow the canonical names with\n"
        " * an FX-native struct.  Private shim macro guard pattern (same\n"
        " * as the other FX types above) keeps the typedef unique if the\n"
        " * shim is ever force-included twice in the same TU. */\n"
        "#undef CLDFB_FILTER_BANK\n"
        "#undef HANDLE_CLDFB_FILTER_BANK\n"
        "#ifndef EVS_FX_SHIM_HAS_CLDFB_FILTER_BANK\n"
        "#define EVS_FX_SHIM_HAS_CLDFB_FILTER_BANK\n"
        "typedef struct\n"
        "{\n"
        "    Word16 no_channels;\n"
        "    Word16 no_col;\n"
        "    Word16 p_filter_length;\n"
        "    CLDFB_TYPE type;\n"
        "\n"
        "    const Word16 *p_filter;\n"
        "\n"
        "    /* rotation vectors */\n"
        "    const Word16 *rot_vec_ana_re;\n"
        "    const Word16 *rot_vec_ana_im;\n"
        "    const Word16 *rot_vec_syn_re;\n"
        "    const Word16 *rot_vec_syn_im;\n"
        "\n"
        "    /* memory helper states */\n"
        "    Word16 *memory;\n"
        "    Word16 memory_length;\n"
        "\n"
        "    /* main filter state (FX name; float side calls this\n"
        "     * `cldfb_state`).  Cleared in lib_enc/swb_pre_proc_fx.c:423\n"
        "     * and written by cldfbInitAnalysisFilterBank /\n"
        "     * cldfbInitSynthesisFilterBank via the `pFilterStates` arg\n"
        "     * (see prot_fx.h:9940, 9957). */\n"
        "    Word16 *FilterStates;\n"
        "\n"
        "    /* other parameters.  scale is Word16 not float because\n"
        "     * initFdCngEnc / initFdCngDec both take a Word16 scale\n"
        "     * argument.  usb / lsb are FX-only fields populated by\n"
        "     * cldfbInitAnalysisFilterBank from its `lsb` / `usb` args\n"
        "     * (prot_fx.h:9942-9943) and read by lib_dec/evs_dec_fx.c\n"
        "     * (st_fx->cldfbAna_fx->usb, st_fx->cldfbSyn_fx->lsb) and\n"
        "     * lib_dec/acelp_core_dec_fx.c (st_fx->cldfbAna_fx->usb *\n"
        "     * st_fx->cldfbAna_fx->no_col compared against\n"
        "     * st_fx->L_frame_fx). */\n"
        "    Word16 bandsToZero;\n"
        "    Word16 nab;\n"
        "    Word16 usb;\n"
        "    Word16 lsb;\n"
        "    Word16 scale;\n"
        "}\n"
        "CLDFB_FILTER_BANK;\n"
        "\n"
        "typedef CLDFB_FILTER_BANK *HANDLE_CLDFB_FILTER_BANK;\n"
        "#endif\n"
        "\n"
        "/* Undefine the FD_CNG remap macros now that stat_com.h has been\n"
        " * processed. The float FD_CNG_SETUP / FD_CNG_COM are still\n"
        " * defined in the TU under the aliases `evs_fx_fdcngsetup_f` /\n"
        " * `evs_fx_fdcngcom_f` (which nothing else references), so we can\n"
        " * safely shadow the canonical names with FX-native structs.\n"
        " * Private shim macro guards (same pattern as the other FX types\n"
        " * above) keep the typedefs unique if the shim is ever\n"
        " * force-included twice in the same TU.  The layout mirrors the\n"
        " * FX consumption pattern in lib_dec/{acelp_core_dec_fx.c,\n"
        " * acelp_core_switch_dec_fx.c, amr_wb_dec_fx.c,\n"
        " * core_switching_dec_fx.c, evs_dec_fx.c} and the FX setup\n"
        " * tables in lib_com/rom_com_fx.c (FdCngSetup_nb/wb1/wb2/wb3/\n"
        " * swb1/swb2 use `sizeof(...)/sizeof(Word16)` and `Word16`\n"
        " * partition tables, which the float struct's `int` scalars and\n"
        " * `const int*` arrays would not accept). */\n"
        "#undef FD_CNG_SETUP\n"
        "#undef FD_CNG_COM\n"
        "#undef HANDLE_FD_CNG_COM\n"
        "#ifndef EVS_FX_SHIM_HAS_FD_CNG_SETUP\n"
        "#define EVS_FX_SHIM_HAS_FD_CNG_SETUP\n"
        "typedef struct\n"
        "{\n"
        "    Word16 fftlen;                       /* FFT length */\n"
        "    Word16 stopFFTbin;                   /* Number of FFT bins to be actually processed */\n"
        "    Word16 numPartitions;                /* Number of partitions */\n"
        "    const Word16 *sidPartitions;         /* Upper boundaries for grouping (sub)bands into partitions when transmitting SID frames */\n"
        "    Word16 numShapingPartitions;         /* Number of partitions */\n"
        "    const Word16 *shapingPartitions;     /* Upper boundaries for grouping (sub)bands into partitions for shaping at the decoder */\n"
        "} FD_CNG_SETUP;\n"
        "#endif\n"
        "#ifndef EVS_FX_SHIM_HAS_FD_CNG_COM\n"
        "#define EVS_FX_SHIM_HAS_FD_CNG_COM\n"
        "typedef struct\n"
        "{\n"
        "    FD_CNG_SETUP FdCngSetup;\n"
        "\n"
        "    Word16 numSlots;                    /* Number of time slots in CLDFB matrix */\n"
        "    Word16 regularStopBand;             /* Number of CLDFB bands to be considered */\n"
        "    Word16 numCoreBands;                /* Number of core bands to be decomposed into FFT subbands */\n"
        "    Word16 stopBand;                    /* Total number of (sub)bands to be considered */\n"
        "    Word16 startBand;                   /* First (sub)band to be considered */\n"
        "    Word16 stopFFTbin;                  /* Total number of FFT subbands */\n"
        "    Word16 frameSize;                   /* Frame size in samples */\n"
        "    Word16 fftlen;                      /* FFT length used for the decomposition */\n"
        "    /* FX-only scaling factor applied in lib_dec/acelp_core_dec_fx.c:1047\n"
        "     * to scale the overlap-add tail.  The float FD_CNG_COM does not\n"
        "     * carry this field. */\n"
        "    Word16 fftlenFac;\n"
        "\n"
        "    /* Synthesis overlap buffers.  Accessed as Word16* in\n"
        "     * lib_dec/amr_wb_dec_fx.c (`set16_fx(..., olapBufferSynth2, ...)`,\n"
        "     * `syn_fx[i] = add(syn_fx[i], shr_r(olapBufferSynth2[i+5*L_FRAME/4], ...))`),\n"
        "     * lib_dec/core_switching_dec_fx.c (`lerp(olapBufferSynth2, ...)`,\n"
        "     * `mult_r(olapBufferSynth[n], 20480)`), and\n"
        "     * lib_dec/evs_dec_fx.c (set16_fx / shr_r).  The float struct\n"
        "     * declares them as `float olapBufferSynth[FFTLEN]` / `float\n"
        "     * olapBufferSynth2[FFTLEN]`, so the warning currently seen at\n"
        "     * acelp_core_dec_fx.c:1065 / evs_dec_fx.c:977,978,1030 goes\n"
        "     * away once they are Word16 here. */\n"
        "    Word16 timeDomainBuffer[L_FRAME16k];\n"
        "    Word16 olapBufferSynth[FFTLEN];\n"
        "    Word16 olapBufferSynth2[FFTLEN];\n"
        "\n"
        "    /* Noise-estimation random seed.  rand_gauss() in prot_fx.h takes\n"
        "     * `Word16 *seed` and writebacks the updated seed in place. */\n"
        "    Word16 seed;\n"
        "\n"
        "    /* Number of active partitions (set by initPartitions in\n"
        "     * lib_dec/fd_cng_dec.c -- that source is float-only and is not\n"
        "     * linked into evs-lib-dec-fx, so the field is set indirectly by\n"
        "     * the encoder or by the float configure path that runs at\n"
        "     * startup; we still need the slot in the struct so the FX\n"
        "     * decoders can read it). */\n"
        "    Word16 npart;\n"
        "\n"
        "    /* VAD state surfaced through noisy_speech_detection().  All three\n"
        "     * fields are read/written by lib_dec/{acelp_core_dec_fx.c,\n"
        "     * amr_wb_dec_fx.c, evs_dec_fx.c} (mult_r/add 0.99 / 0.01 in\n"
        "     * Q15).  The float struct uses `short` / `float` here. */\n"
        "    Word16 active_frame_counter;\n"
        "    Word16 flag_noisy_speech;\n"
        "    Word16 likelihood_noisy_speech;\n"
        "\n"
        "    /* Previous-frame type (set from `m_frame_type` in\n"
        "     * lib_dec/{amr_wb_dec_fx.c, evs_dec_fx.c}).  Float struct\n"
        "     * uses `int frame_type_previous`; FX callers use `move16()\n"
        "     * +` so Word16 is correct. */\n"
        "    Word16 frame_type_previous;\n"
        "\n"
        "    /* CNG bitrate.  Assigned from `st_fx->total_brate_fx`\n"
        "     * (lib_dec/core_switching_dec_fx.c:704), which is a Word32\n"
        "     * in the decoder state, so the field must be Word32 to\n"
        "     * avoid narrowing.  CngBandwidth is a small enum value\n"
        "     * (NB/WB/SWB), so Word16 is sufficient. */\n"
        "    Word32 CngBitrate;\n"
        "    Word16 CngBandwidth;\n"
        "\n"
        "    /* LP coefficients of the CNG spectral shape.  Copied into\n"
        "     * `A[M+1]` by `Copy(st_fx->hFdCngDec_fx->hFdCngCom->A_cng, A, M+1)`\n"
        "     * in lib_dec/evs_dec_fx.c:978.  Float struct uses `float A_cng[17]`\n"
        "     * (warning at evs_dec_fx.c:978). */\n"
        "    Word16 A_cng[17];\n"
        "} FD_CNG_COM;\n"
        "typedef FD_CNG_COM *HANDLE_FD_CNG_COM;\n"
        "#endif\n"
        "\n"
        "/* DTFS_STRUCTURE_FX is the FX counterpart of the float\n"
        " * DTFS_STRUCTURE defined above in stat_com.h, and is referenced\n"
        " * by lib_com/prot_fx.h (DTFS_new_fx, DTFS_sub_fx, DTFS_getEngy_fx,\n"
        " * DTFS_poleFilter_fx, copy_phase_fx, ...), lib_com/wi_fx.c\n"
        " * (everywhere: dtfs_fx->a_fx, dtfs_fx->b_fx, dtfs_fx->lag_fx,\n"
        " * dtfs_fx->nH_fx, dtfs_fx->nH_4kHz_fx, dtfs_fx->upper_cut_off_freq_fx,\n"
        " * dtfs_fx->upper_cut_off_freq_of_interest_fx, dtfs_fx->Fs_fx,\n"
        " * dtfs_fx->Q), lib_dec/ppp_dec_fx.c, and lib_enc/ppp_enc_fx.c.\n"
        " * Neither stat_com.h nor any FX-specific header defines it.\n"
        " * The float DTFS_STRUCTURE in stat_com.h uses `float a[MAXLAG_WI]` /\n"
        " * `float b[MAXLAG_WI]` arrays, plus float scalar fields, which are\n"
        " * not usable for fixed-point code; the FX code uses Q-format\n"
        " * `Word16` arrays plus the `Q` field that records the Q-format of\n"
        " * the a/b arrays at run-time (see wi_fx.c: `s = negate(X_fx->Q);`\n"
        " * followed by `shl_r(X_fx->a_fx[i], s)`).  MAXLAG_WI is defined\n"
        " * in lib_com/cnst_fx.h (included earlier in this shim) as\n"
        " * `(PPP_LAG_THRLD/2 + 12)`.  Same private shim macro guard\n"
        " * pattern as the other FX types. */\n"
        "#ifndef EVS_FX_SHIM_HAS_DTFS_STRUCTURE_FX\n"
        "#define EVS_FX_SHIM_HAS_DTFS_STRUCTURE_FX\n"
        "typedef struct\n"
        "{\n"
        "    Word16 a_fx[MAXLAG_WI];\n"
        "    Word16 b_fx[MAXLAG_WI];\n"
        "    Word16 lag_fx;\n"
        "    Word16 nH_fx;\n"
        "    Word16 nH_4kHz_fx;\n"
        "    Word16 upper_cut_off_freq_of_interest_fx;\n"
        "    Word16 upper_cut_off_freq_fx;\n"
        "    Word16 Fs_fx;\n"
        "    Word16 Q;\n"
        "} DTFS_STRUCTURE_FX;\n"
        "#endif\n"
        "\n"
        "/* PvqEntry_fx is the FX counterpart of the float PvqEntry defined\n"
        " * above in stat_com.h, and is referenced by lib_com/prot_fx.h\n"
        " * (mpvq_encode_vec_fx, get_size_mpvq_calc_offset_fx, mpvq_decode_vec_fx),\n"
        " * lib_com/index_pvq_opt_fx.c (entry.dim, entry.k_val, entry.index,\n"
        " * entry.size, entry.lead_sign_ind), lib_enc/pvq_encode_fx.c\n"
        " * (rc_enc_uniform_fx(st_fx, entry.index, entry.size) and\n"
        " * rc_enc_bits_fx(st_fx, UL_deposit_l(entry.lead_sign_ind), 1)),\n"
        " * and lib_dec/pvq_decode_fx.c (rc_dec_uniform_fx(st_fx, entry.size)\n"
        " * assigns into entry.index, and entry.lead_sign_ind is assigned\n"
        " * via `(short)rc_dec_bits_fx(st_fx, 1)`).  The float PvqEntry in\n"
        " * stat_com.h uses `short lead_sign_ind;` / `unsigned int index, size;`\n"
        " * / `short dim, k_val;`, which is not directly usable for the\n"
        " * FX code: rc_dec_uniform_fx returns `UWord32`, rc_dec_bits_fx\n"
        " * returns `Word32` and is cast to `short` for `lead_sign_ind`,\n"
        " * direct_msize_fx / nm_h_prep_opt_fx both return `UWord32` (so\n"
        " * `size` is UWord32), vec2mind_fx writes `UWord32 *index`, and\n"
        " * the `dim` / `k_val` assignments are followed by `move16()`\n"
        " * (so both are Word16).  Same private shim macro guard pattern. */\n"
        "#ifndef EVS_FX_SHIM_HAS_PVQENTRY_FX\n"
        "#define EVS_FX_SHIM_HAS_PVQENTRY_FX\n"
        "typedef struct\n"
        "{\n"
        "    Word16 lead_sign_ind;\n"
        "    UWord32 index;\n"
        "    UWord32 size;\n"
        "    Word16 dim;\n"
        "    Word16 k_val;\n"
        "} PvqEntry_fx;\n"
        "#endif\n"
        "\n"
        "/* TEMPORAL_ENVELOPE_CODING_DECODER_FX and TEMPORAL_ENVELOPE_CODING_ENCODER_FX\n"
        " * are referenced by lib_dec/stat_dec_fx.h (tecDec_fx) and\n"
        " * lib_enc/stat_enc_fx.h (tecEnc), but neither lib_com/stat_com.h\n"
        " * nor any FX-specific header defines them.  A previous shim\n"
        " * release aliased the FX name to the float struct defined by\n"
        " * stat_com.h, but that float struct uses `float` arrays and\n"
        " * lacks the `cldfbExp` field that the FX code accesses in\n"
        " * lib_dec/evs_dec_fx.c (st_fx->tecDec_fx.cldfbExp = add(15, ...)),\n"
        " * so the alias was unsafe and has been replaced with full\n"
        " * FX-native definitions.  C cannot test type existence with\n"
        " * `#ifndef`, so use private shim macro guards.  Constants\n"
        " * CLDFB_NO_COL_MAX, MAX_TEC_SMOOTHING_DEG, DELAY_TEMP_ENV_BUFF_TEC,\n"
        " * and EXT_DELAY_HI_TEMP_ENV are all defined in lib_com/cnst_fx.h\n"
        " * which is included earlier in this shim. */\n"
        "#ifndef EVS_FX_SHIM_HAS_TEMPORAL_ENVELOPE_CODING_DECODER_FX\n"
        "#define EVS_FX_SHIM_HAS_TEMPORAL_ENVELOPE_CODING_DECODER_FX\n"
        "typedef struct\n"
        "{\n"
        "    Word16 cldfbExp;\n"
        "    Word16 loBuffer[CLDFB_NO_COL_MAX + MAX_TEC_SMOOTHING_DEG];\n"
        "    Word16 pGainTemp_m[CLDFB_NO_COL_MAX];\n"
        "    Word16 pGainTemp_e[CLDFB_NO_COL_MAX];\n"
        "} TEMPORAL_ENVELOPE_CODING_DECODER_FX;\n"
        "typedef TEMPORAL_ENVELOPE_CODING_DECODER_FX* HANDLE_TEC_DEC_FX;\n"
        "#endif\n"
        "#ifndef EVS_FX_SHIM_HAS_TEMPORAL_ENVELOPE_CODING_ENCODER_FX\n"
        "#define EVS_FX_SHIM_HAS_TEMPORAL_ENVELOPE_CODING_ENCODER_FX\n"
        "typedef struct\n"
        "{\n"
        "    Word16 loBuffer[CLDFB_NO_COL_MAX + MAX_TEC_SMOOTHING_DEG + DELAY_TEMP_ENV_BUFF_TEC];\n"
        "    Word16 loTempEnv[CLDFB_NO_COL_MAX];\n"
        "    Word16 loTempEnv_ns[CLDFB_NO_COL_MAX];\n"
        "    Word16 hiTempEnv[CLDFB_NO_COL_MAX + DELAY_TEMP_ENV_BUFF_TEC + EXT_DELAY_HI_TEMP_ENV];\n"
        "    Word16 tranFlag;\n"
        "    Word16 corrFlag;\n"
        "} TEMPORAL_ENVELOPE_CODING_ENCODER_FX;\n"
        "typedef TEMPORAL_ENVELOPE_CODING_ENCODER_FX* HANDLE_TEC_ENC_FX;\n"
        "#endif\n"
        "\n"
        "/* BITSTREAM_FX / PBITSTREAM_FX and ARCODEC_FX / PARCODEC_FX are\n"
        " * referenced by lib_com/tcq_position_arith_fx.c (arithmetic\n"
        " * codec functions: bitstream_save_bit, bitstream_load_bit,\n"
        " * ar_encoder_start_fx, ar_decoder_start_fx, ...), by\n"
        " * lib_enc/tcq_core_enc_fx.c and lib_dec/tcq_core_dec_fx.c\n"
        " * (as local ARCODEC_FX arenc_fx, BITSTREAM_FX bs_fx, plus\n"
        " * the AR-instruction-pointer field `.bsInst` and the BS\n"
        " * fields `.curPos`, `.numByte`, `.numbits`, `.maxBytes`,\n"
        " * `.buf[i]`), and by lib_com/prot_fx.h (function prototypes\n"
        " * `ar_encoder_start_fx( PARCODEC_FX, PBITSTREAM_FX, ... )`).\n"
        " * Neither lib_com/stat_com.h nor any FX-specific header\n"
        " * defines them.  The float stat_com.h defines `BITSTREAM`\n"
        " * and `ARCODEC` (no _FX suffix) with float-side field types\n"
        " * (`signed char curPos`, `unsigned int numByte`, `unsigned int\n"
        " * numbits`, ...) - the FX code expects `Word16 curPos`,\n"
        " * `Word32 numByte`, `Word32 numbits`, so the float types are\n"
        " * not reusable.  Define the FX variants here, with the same\n"
        " * private shim macro guard pattern used for the CLDFB types.\n"
        " * MAX_SIZEBUF_PBITSTREAM is defined in lib_com/cnst_fx.h\n"
        " * (included earlier in this shim) and is 1024, large enough\n"
        " * for the full bitstream buffer. */\n"
        "#ifndef EVS_FX_SHIM_HAS_BITSTREAM_FX\n"
        "#define EVS_FX_SHIM_HAS_BITSTREAM_FX\n"
        "typedef struct\n"
        "{\n"
        "    UWord8 buf[MAX_SIZEBUF_PBITSTREAM];\n"
        "    Word16 curPos;\n"
        "    Word32 numByte;\n"
        "    Word32 numbits;\n"
        "    UWord32 maxBytes;\n"
        "} BITSTREAM_FX;\n"
        "typedef BITSTREAM_FX* PBITSTREAM_FX;\n"
        "#endif\n"
        "#ifndef EVS_FX_SHIM_HAS_ARCODEC_FX\n"
        "#define EVS_FX_SHIM_HAS_ARCODEC_FX\n"
        "typedef struct\n"
        "{\n"
        "    PBITSTREAM_FX bsInst;\n"
        "    Word32 low;\n"
        "    Word32 high;\n"
        "    UWord32 value;\n"
        "    Word16 bits_to_follow;\n"
        "    Word32 num_bits;\n"
        "    Word32 max_bits;\n"
        "} ARCODEC_FX;\n"
        "typedef ARCODEC_FX* PARCODEC_FX;\n"
        "#endif\n"
        "\n"
        "/* Mpy_32_16 collision: lib_com/basop_mpy.h declares a 2-arg\n"
        " * helper `Mpy_32_16(Word32, Word16)` defined in lib_com/basop_mpy.c,\n"
        " * while basic_math/oper_32b.h declares a 3-arg helper\n"
        " * `Mpy_32_16(Word16 hi, Word16 lo, Word16 n)` defined in\n"
        " * basic_math/oper_32b.c.  Both names are `Mpy_32_16`, so C\n"
        " * cannot keep both prototypes in the same translation unit.\n"
        " *\n"
        " * Resolution: rename the 2-arg helper to `evs_fx_Mpy_32_16_2arg`\n"
        " * so the 3-arg helper keeps the bare `Mpy_32_16` name.  The\n"
        " * rename is implemented as a forced-include-time object-like\n"
        " * macro, scoped only to the moment `basop_mpy.h` is processed.\n"
        " * The basop_mpy.h include guard (`__BASOP_MPY_H`) is set, so any\n"
        " * later `#include \"basop_mpy.h\"` from sources that need the\n"
        " * 2-arg prototype is a no-op and the renamed prototype stays\n"
        " * visible.  After the include we `#undef` the rename and pull\n"
        " * in oper_32b.h so the 3-arg prototype from basic_math/oper_32b.h\n"
        " * is what every FX source actually sees for bare `Mpy_32_16`\n"
        " * calls.\n"
        " *\n"
        " * Normal FX codec sources (gs_gains_fx.c, swb_tbe_com_fx.c,\n"
        " * swb_bwe_com_lr_fx.c, wi_fx.c, tools_fx.c, ...) use the bare\n"
        " * `Mpy_32_16` name to call the 3-arg helper from oper_32b.c, so\n"
        " * the undef + oper_32b.h include is required.  The other FX\n"
        " * source files (oper_32b.c, math_32.c, hq2_noise_inject_fx.c,\n"
        " * ...) also reach the 3-arg prototype through their own\n"
        " * `#include \"oper_32b.h\"` (directly or via math_op.h /\n"
        " * basic_op's stl.h), but those #includes become no-ops because\n"
        " * the `_OPER_32b_H` guard is already set by this shim.\n"
        " *\n"
        " * In the parent-only FX build, the lib_com helper sources that\n"
        " * actually use the 2-arg helper (basop_mpy.c, basop_com_lpc.c,\n"
        " * basop_lsf_tools.c) ARE compiled into EVS_FX_BASOP_COM_SOURCES,\n"
        " * and they each carry the per-source compile definition\n"
        " * `TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME` so the 2-arg rename stays\n"
        " * active in those TUs and their `Mpy_32_16(x, y)` calls resolve\n"
        " * against the renamed `evs_fx_Mpy_32_16_2arg` symbol from\n"
        " * basop_mpy.c.  basop_util.c is also compiled under a narrower\n"
        " * TELEPHONY_EVS_FX_BASOP_UTIL_SHIM carve-out, while\n"
        " * basop_tcx_utils.c remains excluded because it includes float\n"
        " * stat_dec/prot/rom_com headers and has a BASOP_cfft signature\n"
        " * mismatch.  The 2-arg\n"
        " * prototype is still recorded in the shim because prot_fx.h\n"
        " * transitively reaches basop_util.h, which itself includes\n"
        " * basop_mpy.h; without the rename the 2-arg prototype would\n"
        " * poison the 3-arg prototype coming from oper_32b.h.  Keeping\n"
        " * the include under the rename sidesteps that without any\n"
        " * per-source KEEP conditional on the read-only 3GPP source.\n"
        " *\n"
        " * The macro is a single-token rename, so `Mpy_32_16_1`,\n"
        " * `Mpy_32_16_r`, `Mpy_32_16_ss`, `Mpy_32_16_uu` and the struct\n"
        " * member `Mpy_32_16_ss` are unaffected (the preprocessor only\n"
        " * matches whole identifiers, and the trailing `_...` is part of\n"
        " * those identifiers). */\n"
        "/* The 2-arg rename is always active while `basop_mpy.h` is\n"
        " * processed, so its 2-arg `Mpy_32_16(Word32, Word16)` prototype\n"
        " * is recorded under `evs_fx_Mpy_32_16_2arg` (which is what the\n"
        " * lib_com/basop_*.c helpers actually call).  Whether the 3-arg\n"
        " * `Mpy_32_16(Word16 hi, Word16 lo, Word16 n)` prototype from\n"
        " * basic_math/oper_32b.h is pulled in afterwards depends on the\n"
        " * per-source `TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME` compile\n"
        " * definition: the included helper TUs (basop_mpy.c,\n"
        " * basop_com_lpc.c, basop_lsf_tools.c) define it, so the\n"
        " * `#undef Mpy_32_16` + `#include \"oper_32b.h\"` block is\n"
        " * skipped and the bare `Mpy_32_16` name in those TUs keeps\n"
        " * pointing at the renamed 2-arg symbol.  Normal FX codec\n"
        " * sources (gs_gains_fx.c, swb_tbe_com_fx.c, swb_bwe_com_lr_fx.c,\n"
        " * wi_fx.c, tools_fx.c, ...) do NOT define the macro, so the\n"
        " * undef + oper_32b.h include runs and they see the 3-arg\n"
        " * prototype - matching the 3-arg Mpy_32_16 they actually call\n"
        " * in basic_math/oper_32b.c.  basop_util.c gets its own separate\n"
        " * per-source KEEP_2ARG_MPY_NAME + BASOP_UTIL_SHIM property below;\n"
        " * basop_tcx_utils.c remains excluded because it includes the float\n"
        " * stat_dec.h / prot.h / rom_com.h headers and has a BASOP_cfft\n"
        " * signature mismatch. */\n"
        "#define Mpy_32_16 evs_fx_Mpy_32_16_2arg\n"
        "#include \"basop_mpy.h\"\n"
        "#ifndef TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME\n"
        "#undef Mpy_32_16\n"
        "#include \"oper_32b.h\"\n"
        "#endif\n"
        "\n"
        "/* ------------------------------------------------------------------\n"
        " * FX control-flow macro override (parent-only fix).\n"
        " *\n"
        " * external/3gpp-evs/lib_com/control.h (read-only) defines the\n"
        " * control-flow macros (FOR, WHILE, DO, IF, ELSE, SWITCH, CONTINUE,\n"
        " * BREAK, GOTO) with a WMOPS-counter hook. Its WHILE macro is:\n"
        " *\n"
        " *   #define WHILE( a) if (incrFlcWhile(), 0); else while(a)\n"
        " *\n"
        " * which is broken for the `DO { ... } WHILE(cond);` pattern. The\n"
        " * `DO` macro expands to plain `do`, so the full construct becomes\n"
        " * `do { ... } if (incrFlcWhile(), 0); else while(cond);` -- the\n"
        " * `else` has no matching `if`, producing C2181 (\"illegal else\n"
        " * without matching if\") in MSVC and a generic \"syntax error\" in\n"
        " * GCC/Clang. This is exactly what we see in lsf_tools_fx.c:423,\n"
        " * pvq_com_fx.c:230, swb_bwe_com_lr_fx.c:2218 / 2497, etc.\n"
        " *\n"
        " * external/3gpp-evs/basic_op/control.h (also read-only) has the\n"
        " * correct WMOPS=0 form that maps every macro to its plain C\n"
        " * keyword. We cannot rely on it being the one processed, because:\n"
        " *   - lib_com/stl.h lives in the same directory as the *_fx.c\n"
        " *     sources, so `#include \"stl.h\"` from a lib_com/ source file\n"
        " *     resolves to lib_com/stl.h (the `\"...\"` form searches the\n"
        " *     source file's directory first), not basic_op/stl.h;\n"
        " *   - lib_com/stl.h then includes lib_com/control.h, whose body\n"
        " *     sets the shared _CONTROL_H guard, which in turn suppresses\n"
        " *     the safe basic_op/control.h the moment basic_op/stl.h tries\n"
        " *     to pull it in. Net result: only the broken macros survive.\n"
        " *\n"
        " * Fix: pre-define the shared _CONTROL_H guard here in the shim\n"
        " * (the shim is force-included at the very start of every FX TU,\n"
        " * before the source's own `#include \"stl.h\"`), so the bodies of\n"
        " * both lib_com/control.h and basic_op/control.h are skipped when\n"
        " * stl.h later tries to include them. We then provide the safe\n"
        " * no-counter forms ourselves. WMOPS instrumentation is sacrificed\n"
        " * (we never enable WMOPS in the build), but the C keyword\n"
        " * semantics of FOR/WHILE/DO/IF/ELSE/SWITCH/CONTINUE/BREAK/GOTO are\n"
        " * preserved, so the existing source code that uses the all-caps\n"
        " * macros (e.g. `IF (sub(n,5) > 0) { ... } ELSE { ... }`,\n"
        " * `DO { ... } WHILE(cond);`, `FOR (i = 0; i < n; i++)`) keeps\n"
        " * working. All nine macros from lib_com/control.h are overridden.\n"
        " *\n"
        " * The lib_com/control.h body also defines `static __inline`\n"
        " * helpers incrFor / incrFlcWhile / incrIf / incrSwitch. Skipping\n"
        " * the body is safe because the safe-keyword macros do not call\n"
        " * any of them. A repo-wide grep confirms the helpers are only\n"
        " * referenced from the two control.h files themselves.\n"
        " *\n"
        " * `_CONTROL_H` is guarded with `#ifndef` because the new\n"
        " * `#include \"enh40.h\"` block above pulls in `basic_op/enh40.h`\n"
        " * -> `basic_op/stl.h` -> `basic_op/control.h` for source files\n"
        " * under `basic_op/`, which sets `_CONTROL_H` to the safe WMOPS=0\n"
        " * keyword-only form.  Without the `#ifndef` guard, this\n"
        " * `#define _CONTROL_H` below would be a redefinition of the\n"
        " * same macro to the same value, triggering MSVC C4005\n"
        " * (`macro redefinition`).  When `_CONTROL_H` is not yet set\n"
        " * (e.g. for `lib_com/` source files where the shim's\n"
        " * `#include \"enh40.h\"` resolves to `lib_com/enh40.h` ->\n"
        " * `lib_com/stl.h` -> `lib_com/control.h`, which DOES set\n"
        " * `_CONTROL_H` to the broken incrFlcWhile form, which we\n"
        " * then immediately neutralise via the `#undef FOR` /\n"
        " * `#undef WHILE` / ... block below), the `#ifndef` simply\n"
        " * does nothing and the macro is defined for the first time.\n"
        " * ------------------------------------------------------------------ */\n"
        "#ifndef _CONTROL_H\n"
        "#define _CONTROL_H\n"
        "#endif\n"
        "#undef FOR\n"
        "#undef WHILE\n"
        "#undef DO\n"
        "#undef IF\n"
        "#undef ELSE\n"
        "#undef SWITCH\n"
        "#undef CONTINUE\n"
        "#undef BREAK\n"
        "#undef GOTO\n"
        "#define FOR(a) for(a)\n"
        "#define WHILE(a) while(a)\n"
        "#define DO do\n"
        "#define IF(a) if(a)\n"
        "#define ELSE else\n"
        "#define SWITCH(a) switch(a)\n"
        "#define CONTINUE continue\n"
        "#define BREAK break\n"
        "#define GOTO goto\n"
        "\n"
        "/* ------------------------------------------------------------------\n"
        " * basop_util.c / rom_basop_util.c FX isolation block (Option A).\n"
        " *\n"
        " * Activated only when the per-source compile definition\n"
        " * `TELEPHONY_EVS_FX_BASOP_UTIL_SHIM` is set (see the\n"
        " * `set_property(SOURCE ... APPEND PROPERTY COMPILE_DEFINITIONS ...)`\n"
        " * call above for lib_com/basop_util.c).  Every other FX TU\n"
        " * compiled by the parent-only build skips this block entirely.\n"
        " *\n"
        " * Why this block exists:\n"
        " *\n"
        " *   - lib_com/basop_util.c (line 10) includes the FLOAT\n"
        "     `rom_com.h` directly.  The float `rom_com.h` body declares\n"
        "     tables with float-side types (the FX counterpart lives in\n"
        "     lib_com/rom_basop_util.c).  Pre-defining the float header's\n"
        "     include guard `ROM_COM_H` here makes the body of the float\n"
        "     `rom_com.h` a no-op, so the FX-side tables from\n"
        "     `rom_basop_util.c` (declared in `rom_basop_util.h`) are the\n"
        "     only ones the TU sees.\n"
        " *\n"
        " *   - `basop_util.c` and the float headers it pulls in\n"
        "     transitively (cnst.h -> ..., options.h -> ...) do not\n"
        "     reference stat_dec.h / stat_enc.h today, but pre-defining\n"
        "     `STAT_DEC_H` / `STAT_ENC_H` here is cheap insurance: if a\n"
        "     future 3GPP revision adds a transitive include that reaches\n"
        "     them, the float bodies stay skipped without an extra shim\n"
        "     edit.\n"
        " *\n"
        " *   - lib_com/rom_basop_util.h declares the FX-side tables\n"
        "     (`SineTable512`/`SineTable480`/`SineTable400`/\n"
        "     `SineTable384`/`SineTable320`, `ldCoeff`, `exp2*_tab_long`,\n"
        "     `ldIntCoeff`, `invTable`, `InvIntTable`, `sqrtTable`,\n"
        "     `invSqrtTable`, `BASOP_util_normReciprocal`,\n"
        "     `f_atan_expand_range`) and the prototypes `BASOP_getTables`\n"
        "     / `getSineWindowTable`.  Including `rom_basop_util.h` here\n"
        "     makes those prototypes visible to basop_util.c regardless\n"
        "     of whether its source-level `#include \"rom_com.h\"` has\n"
        "     been short-circuited by the ROM_COM_H predefine above.\n"
        " *\n"
        " *   - basop_util.c (line 486) defines `SINETAB SineTable512_fx`,\n"
        "     but the FX-side symbol defined in `rom_basop_util.c` is\n"
        "     `SineTable512` (the read-only 3GPP source uses the `_fx`\n"
        "     suffix only in basop_tcx_utils.c, not in basop_util.c -\n"
        "     upstream naming inconsistency we cannot fix).  Aliasing\n"
        "     `SineTable512_fx -> SineTable512` here so the SINETAB\n"
        "     reference inside basop_util.c resolves against the actual\n"
        "     definition from `rom_basop_util.c`.  The 480/400/384/320\n"
        "     siblings are also declared in `rom_basop_util.h`, but\n"
        "     `basop_util.c` itself does not reference them (a grep of\n"
        "     the source confirms only `SineTable512_fx` is used), so\n"
        "     aliasing just `SineTable512_fx` is sufficient and avoids\n"
        "     pulling in additional aliases that could collide with\n"
        "     `basop_tcx_utils.c`'s usage of `SineTable512_fx` if that\n"
        "     file is ever added back in a future step.\n"
        " *\n"
        " * No `prot_fx.h` include is added here: basop_util.c does not\n"
        " * include `prot.h` directly, and the FX-side prototypes it\n"
        " * needs (the BASOP_* / sine-table helpers) are all declared in\n"
        " * `basop_util.h` + `rom_basop_util.h` (the latter included\n"
        " * below).  Adding `prot_fx.h` here would re-introduce the\n"
        " * float `stat_dec.h` / `stat_enc.h` include chain that this\n"
        " * block is explicitly trying to suppress.\n"
        " * ------------------------------------------------------------------ */\n"
        "#ifdef TELEPHONY_EVS_FX_BASOP_UTIL_SHIM\n"
        "/* Suppress the float lib_com/rom_com.h body.  Its guard is\n"
        " * ROM_COM_H, so basop_util.c's own `#include \"rom_com.h\"`\n"
        " * becomes a no-op.  The FX-side tables are declared by\n"
        " * `rom_basop_util.h` (included just below) and defined in\n"
        " * `rom_basop_util.c` (compiled into EVS_FX_BASOP_COM_SOURCES). */\n"
        "#ifndef ROM_COM_H\n"
        "#define ROM_COM_H\n"
        "#endif\n"
        "/* Defensive: pre-define stat_dec.h / stat_enc.h guards so a\n"
        " * transitive include chain that reaches them in the future\n"
        " * is a no-op.  STAT_DEC_H guards lib_dec/stat_dec.h and\n"
        " * STAT_ENC_H guards lib_enc/stat_enc.h.  PROT_H is intentionally\n"
        " * NOT pre-defined: basop_util.c does not include prot.h, and\n"
        " * pre-defining it would mask legitimate future compile errors\n"
        " * (e.g. if someone adds a `#include \"prot.h\"` directly). */\n"
        "#ifndef STAT_DEC_H\n"
        "#define STAT_DEC_H\n"
        "#endif\n"
        "#ifndef STAT_ENC_H\n"
        "#define STAT_ENC_H\n"
        "#endif\n"
        "/* Pull in the FX-side table declarations from rom_basop_util.h\n"
        " * (the read-only header that ships alongside rom_basop_util.c).\n"
        " * This makes `extern const Word16 SineTable512[257];` /\n"
        " * `extern const PWord16 SineTable512[];` / `BASOP_getTables` /\n"
        " * `getSineWindowTable` etc. visible to basop_util.c without\n"
        " * going through the float `rom_com.h`. */\n"
        "#include \"rom_basop_util.h\"\n"
        "/* basop_util.c uses `SineTable512_fx` as the SINETAB macro value\n"
        " * (see line 486), but the FX-side definition in rom_basop_util.c\n"
        " * is `SineTable512`.  Token-rewrite `SineTable512_fx` to the\n"
        " * canonical FX-side name so the SINETAB reference in\n"
        " * basop_util.c resolves against the actual symbol. */\n"
        "#define SineTable512_fx SineTable512\n"
        "#endif /* TELEPHONY_EVS_FX_BASOP_UTIL_SHIM */\n"
        "\n"
        "#endif /* EVS_FX_TYPEDEF_SHIM_H */\n"
    )

    # ------------------------------------------------------------------
    # Compile-time defines that suppress the float headers.  This list
    # is intentionally empty: TYPEDEF_H, _TYPEDEF_H, and CNST_H are all
    # pre-defined inside the FX typedef shim (see EVS_FX_TYPEDEF_SHIM).
    # They must be visible *before* the very first
    # `#include "typedef.h"` / `#include "cnst.h"` is reached, and the
    # shim is itself the mechanism that pre-includes typedefs.h,
    # cnst_fx.h, and stat_com.h.  Defining them *both* via the shim
    # and via -DCNST_H would trigger a `macro redefinition` warning
    # because the shim is processed first (force-include) and the
    # command-line `-D` would arrive later with the same value; keeping
    # them in one place (the shim) is cleaner and warning-free.
    #
    # STAT_COM_H is intentionally NOT pre-defined anywhere (neither
    # here nor in the shim): there is no `stat_com_fx.h` companion, so
    # the shared lib_com/stat_com.h is the canonical source for PFSTAT,
    # IGF_INFO, FD_CNG_COM, CLDFB_FILTER_BANK, TEC_DEC, TEC_ENC,
    # TCX_config, ...  Letting stat_com.h set its own guard naturally
    # keeps the contract simple.  When stat_com.h is processed, its own
    # `#include "cnst.h"` and `#include "typedef.h"` are no-ops thanks
    # to the CNST_H and TYPEDEF_H predefines inside the shim.
    # ------------------------------------------------------------------
    set(EVS_FX_FLOAT_SUPPRESS_DEFS
    )

    # ------------------------------------------------------------------
    # Include dirs for the three FX targets.  Order matters: the shim
    # dir is searched first so that any same-name header placed there
    # would win, then basic_op (so `#include \"typedefs.h\"` inside the
    # shim itself resolves to the FX type set), then basic_math, then
    # the usual lib_com / lib_enc / lib_dec.
    # ------------------------------------------------------------------
    set(EVS_FX_INCLUDE_DIRS
        "${EVS_FX_SHIM_DIR}"
        "${EVS_ROOT}/basic_op"
        "${EVS_ROOT}/basic_math"
        ${EVS_INCLUDE_DIRS}
    )

    # ------------------------------------------------------------------
    # Helper: per-target FX settings.  We define a function that
    # attaches the include dirs, compile definitions, force-include
    # shim, MSVC warning suppressions, and the wb_vad renames to the
    # three FX targets so the per-target block stays short.
    # ------------------------------------------------------------------
    function(evs_configure_fx_target TGT)
        target_include_directories(${TGT} PUBLIC ${EVS_FX_INCLUDE_DIRS})
        target_compile_definitions(${TGT} PRIVATE ${EVS_FX_FLOAT_SUPPRESS_DEFS})
        # Rename wb_vad / wb_vad_init to evs_wb_vad* (see header comment above)
        target_compile_definitions(${TGT} PRIVATE ${EVS_WB_VAD_RENAME_DEFS})

        # Force-include the FX typedef shim.  MSVC uses /FI<path> (no
        # space); GCC/Clang use -include <path> (with space).
        if(MSVC)
            target_compile_options(${TGT} PRIVATE "/FI${EVS_FX_TYPEDEF_SHIM}")
        else()
            target_compile_options(${TGT} PRIVATE "-include" "${EVS_FX_TYPEDEF_SHIM}")
        endif()

        if(MSVC)
            target_compile_definitions(${TGT} PRIVATE _CRT_SECURE_NO_WARNINGS)
            target_compile_options(${TGT} PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
        endif()
    endfunction()

    # ------------------------------------------------------------------
    # evs-lib-com-fx: shared FX support library.  All encoder/decoder
    # FX libs PUBLIC-link to it, so this is the only target that needs
    # the non-*_fx.c basic-operator / basic-math / lib_com basop
    # sources.  Putting them here avoids duplicate symbol definitions
    # across static libraries.
    # ------------------------------------------------------------------
    set(EVS_FX_BASOP_COM_SOURCES
        # basic_op/ - the new ITU-T G.191 fixed-point primitives.
        # control.c and count.c are intentionally NOT included: WMOPS
        # is off in this build, the shim overrides the FOR/WHILE/DO/...
        # control-flow macros, and the WMOPS-conditional bodies of
        # control.c / count.c would only contribute dummy WMOPS_*
        # symbols that clash with the opencore-amrnb / opencore-amrwb
        # counter stubs (and are never called by the FX codec sources).
        "${EVS_ROOT}/basic_op/basop32.c"
        "${EVS_ROOT}/basic_op/enh1632.c"
        "${EVS_ROOT}/basic_op/enh40.c"
        "${EVS_ROOT}/basic_op/enhUL32.c"
        # basic_math/ - 32-bit math / log2 / read-only tables.
        # math_32.c is part of the upstream Workspace_msvc/lib_fx.vcxproj
        # (line 83) but was missing from the parent CMake collector.  It
        # defines Mult_32_16 / Mult_32_32 / Madd_32_16 / Msub_32_16
        # (Word32 / Word16 FX helpers) that the FX codec sources expect
        # to find; including it resolves the "unresolved external
        # Mult_32_16" / "Madd_32_16" / "Msub_32_16" / "Mult_32_32" link
        # errors that show up when lib_com/enc/dec FX sources reference
        # them.  The source uses Mpy_32_16 (3-arg, from oper_32b.h) and
        # L_Extract_lc / Mac_32_16 / Msu_32_16 / Mpy_32 (all standard
        # FX primitives from basop32.h), so it builds cleanly under
        # the existing FX shim with no per-source carve-out.
        "${EVS_ROOT}/basic_math/math_op.c"
        "${EVS_ROOT}/basic_math/math_32.c"
        "${EVS_ROOT}/basic_math/oper_32b.c"
        "${EVS_ROOT}/basic_math/log2.c"
        "${EVS_ROOT}/basic_math/rom_basic_math.c"
        # lib_com/ basop helpers (basop_mpy.c, basop_com_lpc.c,
        # basop_lsf_tools.c).  These TUs include lib_com/basop_mpy.h
        # (directly or transitively, via basop_util.h or
        # prot_fx.h -> basop_util.h) and call the 2-arg
        # `Mpy_32_16(Word32, Word16)` helper from basop_mpy.c.  The FX
        # shim renames the 2-arg helper to `evs_fx_Mpy_32_16_2arg`
        # while `basop_mpy.h` is being processed, so we must keep the
        # rename ACTIVE in these TUs.  We do that by giving them the
        # per-source compile definition
        # `TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME` (see the
        # set_property loop below), which makes the
        # shim skip its `#undef Mpy_32_16` + `#include "oper_32b.h"`
        # block and lets `Mpy_32_16(x, y)` calls in these files
        # resolve against the renamed 2-arg symbol.  Normal FX codec
        # sources (gs_gains_fx.c, swb_tbe_com_fx.c, ...) do NOT define
        # the macro, so the shim's undef + oper_32b.h include runs and
        # they see the 3-arg prototype - matching the 3-arg
        # `Mpy_32_16(hi, lo, n)` they actually call from
        # basic_math/oper_32b.c.
        #
        # NOTE: basop_util.c is now included below with a narrow
        # TELEPHONY_EVS_FX_BASOP_UTIL_SHIM carve-out that suppresses
        # only its direct float rom_com.h dependency and redirects it
        # to rom_basop_util.c tables.  basop_tcx_utils.c remains
        # excluded: it includes the float stat_dec.h / prot.h /
        # rom_com.h chain and also calls BASOP_cfft with the 4-argument
        # signature, while the FX-side declaration uses 6 arguments.
        "${EVS_ROOT}/lib_com/basop_mpy.c"
        "${EVS_ROOT}/lib_com/basop_com_lpc.c"
        "${EVS_ROOT}/lib_com/basop_lsf_tools.c"
        # basop_util.c + rom_basop_util.c: opted-in for the FX build
        # under the narrower "Option A" strategy.  basop_util.c directly
        # includes the float `rom_com.h` (see line 10 of basop_util.c)
        # to expose `SineTable512_fx` etc., but the float `rom_com.h`
        # body declares float-side tables (e.g. float `mean_energy`
        # arrays used only by the float helper sources).  The FX
        # typedef shim therefore defines `ROM_COM_H` inside an
        # `#ifdef TELEPHONY_EVS_FX_BASOP_UTIL_SHIM` block (only active
        # for the basop_util.c TU) so the float header body is skipped
        # and the FX-side tables from `rom_basop_util.c` (which itself
        # only includes `rom_basop_util.h` + `stl.h` + `options.h` and
        # no float headers) are used directly.  `STAT_DEC_H` and
        # `STAT_ENC_H` are pre-defined defensively inside the same
        # block in case a transitive include chain reaches them.  The
        # shim also includes `rom_basop_util.h` so the FX-side tables
        # (SineTable512, SineTable480/400/384/320, ldCoeff, exp2*,
        # ldIntCoeff, invTable, InvIntTable, sqrtTable, invSqrtTable,
        # BASOP_util_normReciprocal, f_atan_expand_range) and the
        # prototypes `BASOP_getTables` / `getSineWindowTable` are
        # visible to basop_util.c, and aliases `SineTable512_fx` to
        # `SineTable512` so the `SINETAB` macro in basop_util.c (line
        # 486: `#define SINETAB SineTable512_fx`) lines up with the
        # definition in rom_basop_util.c.
        #
        # basop_tcx_utils.c is INTENTIONALLY NOT added here: it calls
        # `BASOP_cfft` with the 4-argument form but the FX-side
        # declaration (in lib_com/prot_fx.h) is the 6-argument form
        # (an upstream signature mismatch we cannot fix without editing
        # the read-only 3GPP source), and it also includes the float
        # `prot.h` + `cnst.h` + `rom_com.h` headers directly, which
        # triggers the same float-header leakage the shim does not
        # isolate (see the lag_wind.c note below).  basop_tcx_utils.c
        # stays out until those issues are resolved separately.
        "${EVS_ROOT}/lib_com/basop_util.c"
        "${EVS_ROOT}/lib_com/rom_basop_util.c"
        # lag_wind.c / adapt_lag_wind() are float-only (they take
        # `float r[]` autocorrelations and read the `float lag_window_*`
        # tables from rom_com.c).  Upstream Workspace_msvc/common.vcxproj
        # (line 172) ships this in the common *float* project, not in
        # lib_fx.vcxproj.  Confirmed compile-fail under the existing FX
        # shim with errors:
        #   - error C2011: 'SIGNAL_CLASSIFER_MODE': 'enum' 型の再定義
        #     (lib_dec/stat_dec.h:166 redefines the same enum already
        #      declared in lib_com/cnst_fx.h:2207 - the FX shim
        #      includes cnst_fx.h eagerly and the float stat_dec.h
        #      body still runs because stat_dec.h is not suppressed
        #      by the shim, so the enum collides).
        #   - error C2065: 'LGW_MAX': 未定義の識別子です
        #     (defined in float lib_com/cnst.h:230 which the FX shim
        #      suppresses via #define CNST_H, and is not provided by
        #      cnst_fx.h.  Referenced from lib_dec/stat_dec.h:774-776
        #      and lib_com/prot.h:6388.)
        #   - error C2065: 'ODD_DIV_SIZE' / 'INV_TABLE_SIZE' /
        #     'SQRT_TABLE_SIZE': 未定義の識別子です
        #     (defined in float lib_com/cnst.h, referenced from
        #      lib_com/rom_com.h:788, 1218-1220.)
        # Root cause: lag_wind.c includes "prot.h" + "cnst.h" +
        # "rom_com.h" directly, and those float headers transitively
        # pull in float stat_dec.h / stat_enc.h / stat_com.h, which
        # the FX shim does NOT isolate (it only isolates stat_com.h).
        # Fixing this would require suppressing all of stat_dec.h,
        # stat_enc.h, prot.h, cnst.h, and rom_com.h with per-header
        # #define guards in the shim AND providing FX-native
        # replacements for the stat_dec.h / stat_enc.h structs they
        # reference (Decoder_State, Encoder_State, ...), which is
        # well outside the bounds of this task.  Drop it.
    )

    # Per-source compile definition for the lib_com basop helpers:
    # the FX shim checks `TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME` to decide
    # whether to pull in oper_32b.h after basop_mpy.h (see the Mpy_32_16
    # block in EVS_FX_TYPEDEF_SHIM).  Setting it on each helper TU that
    # IS included in EVS_FX_BASOP_COM_SOURCES keeps the 2-arg rename
    # active in that TU so the helper resolves `Mpy_32_16(x, y)`
    # against the renamed symbol instead of the 3-arg prototype from
    # oper_32b.h.  basop_util.c is intentionally handled by a separate
    # set_property block below because it also needs the BASOP_UTIL_SHIM
    # define.  basop_tcx_utils.c is excluded from the FX build.
    #
    # Use set_property(SOURCE <src> APPEND PROPERTY COMPILE_DEFINITIONS
    # <def>) (the APPEND form) so TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME is
    # added to the source's COMPILE_DEFINITIONS list instead of
    # replacing it.  This matches the pattern basop_util.c below uses
    # for its two-definition stack (KEEP_2ARG_MPY_NAME + BASOP_UTIL_SHIM)
    # and keeps the three basop helpers on the same property-append
    # convention, so a future block that adds another compile definition
    # to any of these sources does not silently drop ours.
    foreach(_basop_com_helper_src
            "${EVS_ROOT}/lib_com/basop_mpy.c"
            "${EVS_ROOT}/lib_com/basop_com_lpc.c"
            "${EVS_ROOT}/lib_com/basop_lsf_tools.c")
        set_property(SOURCE "${_basop_com_helper_src}" APPEND
            PROPERTY COMPILE_DEFINITIONS "TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME"
        )
    endforeach()

    # basop_util.c needs both the 2-arg Mpy_32_16 carve-out (so its
    # `Mpy_32_16(x, y)` calls resolve against the renamed
    # `evs_fx_Mpy_32_16_2arg` symbol from basop_mpy.c) AND the
    # `TELEPHONY_EVS_FX_BASOP_UTIL_SHIM` flag that activates the
    # narrow `ROM_COM_H` / `STAT_DEC_H` / `STAT_ENC_H` suppression
    # block + `rom_basop_util.h` include + `SineTable512_fx` alias
    # inside the generated FX typedef shim (see the `#ifdef
    # TELEPHONY_EVS_FX_BASOP_UTIL_SHIM` block at the bottom of
    # EVS_FX_TYPEDEF_SHIM below).  Apply via APPEND so we do not
    # overwrite any COMPILE_DEFINITIONS that may have been set on
    # this source by another part of the build (none today, but the
    # explicit APPEND form is the safest).  rom_basop_util.c is
    # INTENTIONALLY NOT given either flag: it does not call
    # `Mpy_32_16` (it only assigns plain `const Word16` /
    # `const UWord32` / `const Word32` / `const PWord16` arrays) and
    # does not include the float `rom_com.h`, so it does not need
    # the BASOP_UTIL_SHIM block.  KEEP_2ARG_MPY_NAME would still
    # leave its `Mpy_32_16` calls intact, but applying it would mean
    # the basop_util.c-only shim block is not active for it; we just
    # skip the property on rom_basop_util.c entirely.
    set_property(SOURCE "${EVS_ROOT}/lib_com/basop_util.c" APPEND
        PROPERTY COMPILE_DEFINITIONS
            "TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME;TELEPHONY_EVS_FX_BASOP_UTIL_SHIM"
    )

    evs_collect_fx_sources(EVS_LIB_COM_FX_SOURCES
        "${EVS_ROOT}/lib_com"
        ${EVS_FX_EXTRAS_LIB_COM}
    )
    # Append basop support sources that don't follow the *_fx.c
    # convention.  Filter out anything that may have already been
    # picked up by the glob to keep the list de-duplicated.
    foreach(_basop_src ${EVS_FX_BASOP_COM_SOURCES})
        list(FIND EVS_LIB_COM_FX_SOURCES "${_basop_src}" _idx)
        if(_idx EQUAL -1)
            list(APPEND EVS_LIB_COM_FX_SOURCES "${_basop_src}")
        endif()
    endforeach()

    # ------------------------------------------------------------------
    # Mpy_32_16 helper-source carve-out: the lib_com helper sources
    # (basop_mpy.c, basop_com_lpc.c, basop_lsf_tools.c) ARE included in
    # EVS_FX_BASOP_COM_SOURCES above, and they are tagged with the
    # per-source compile definition TELEPHONY_EVS_FX_KEEP_2ARG_MPY_NAME
    # (see the set_property loop above).  The shim
    # (EVS_FX_TYPEDEF_SHIM) checks that macro: if defined, it skips
    # the `#undef Mpy_32_16` + `#include "oper_32b.h"` block and leaves
    # the 2-arg rename `Mpy_32_16 -> evs_fx_Mpy_32_16_2arg` active so
    # the helper's `Mpy_32_16(x, y)` calls resolve against the renamed
    # 2-arg symbol from basop_mpy.c.  For every other FX source (the
    # *_fx.c files in lib_com / lib_enc / lib_dec, plus basic_math/ and
    # basic_op/), the macro is NOT defined, so the shim's undef +
    # oper_32b.h include runs and those TUs see the 3-arg
    # `Mpy_32_16(Word16 hi, Word16 lo, Word16 n)` prototype from
    # basic_math/oper_32b.h, matching the 3-arg helper from
    # basic_math/oper_32b.c that they actually call.
    #
    # basop_util.c is handled separately because it also needs the
    # TELEPHONY_EVS_FX_BASOP_UTIL_SHIM define.  basop_tcx_utils.c remains
    # excluded from the FX build because it pulls in the float
    # stat_dec.h / prot.h / rom_com.h chain and has a BASOP_cfft
    # signature mismatch.
    # ------------------------------------------------------------------

    add_library(evs-lib-com-fx STATIC ${EVS_LIB_COM_FX_SOURCES})
    evs_configure_fx_target(evs-lib-com-fx)
    # evs-lib-com-fx carries the same basic-op / basop_mpy / basop_com_lpc
    # helpers that opencore-amrnb / opencore-amrwb / vo-amrwbenc also
    # define.  Symbols like Isqrt, Deemph2, Dot_product12 are duplicated
    # across the FX lib and the AMR libs but their semantics are
    # bit-equivalent (the opencore variants are the same ITU-T G.191
    # basic-operator primitives), so let the MSVC linker pick one
    # implementation at link time.  Mirrors the existing float
    # evs-lib-com handling (INTERFACE /FORCE:MULTIPLE further up).
    if(MSVC)
        target_link_options(evs-lib-com-fx INTERFACE /FORCE:MULTIPLE)
    endif()

    evs_collect_fx_sources(EVS_LIB_ENC_FX_SOURCES
        "${EVS_ROOT}/lib_enc"
        ${EVS_FX_EXTRAS_LIB_ENC}
    )
    add_library(evs-lib-enc-fx STATIC ${EVS_LIB_ENC_FX_SOURCES})
    target_link_libraries(evs-lib-enc-fx PUBLIC evs-lib-com-fx)
    evs_configure_fx_target(evs-lib-enc-fx)

    evs_collect_fx_sources(EVS_LIB_DEC_FX_SOURCES "${EVS_ROOT}/lib_dec")
    add_library(evs-lib-dec-fx STATIC ${EVS_LIB_DEC_FX_SOURCES})
    target_link_libraries(evs-lib-dec-fx PUBLIC evs-lib-com-fx)
    evs_configure_fx_target(evs-lib-dec-fx)
endif()
