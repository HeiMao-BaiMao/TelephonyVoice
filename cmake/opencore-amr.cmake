set(OPENCORE_AMR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/opencore-amr")
set(OC_BASE "${OPENCORE_AMR_ROOT}/opencore")
set(AMR_BASE "${OC_BASE}/codecs_v2/audio/gsm_amr")
set(OSCL_DIR "${OPENCORE_AMR_ROOT}/oscl")

# ---------------------------------------------------------------------------
# opencore-amrnb
# ---------------------------------------------------------------------------

set(AMRNB_DEC_DIR "${AMR_BASE}/amr_nb/dec")
set(AMRNB_ENC_DIR "${AMR_BASE}/amr_nb/enc")
set(AMRNB_COMMON_DIR "${AMR_BASE}/amr_nb/common")

file(GLOB AMRNB_DEC_SOURCES "${AMRNB_DEC_DIR}/src/*.cpp")
file(GLOB AMRNB_ENC_SOURCES "${AMRNB_ENC_DIR}/src/*.cpp")
file(GLOB AMRNB_COMMON_SOURCES "${AMRNB_COMMON_DIR}/src/*.cpp")

# Exclude files as per Makefile.am
list(REMOVE_ITEM AMRNB_DEC_SOURCES
    "${AMRNB_DEC_DIR}/src/decoder_gsm_amr.cpp"
    "${AMRNB_DEC_DIR}/src/pvgsmamrdecoder.cpp"
)

list(REMOVE_ITEM AMRNB_ENC_SOURCES
    "${AMRNB_ENC_DIR}/src/gsmamr_encoder_wrapper.cpp"
)

list(REMOVE_ITEM AMRNB_COMMON_SOURCES
    "${AMRNB_COMMON_DIR}/src/bits2prm.cpp"
    "${AMRNB_COMMON_DIR}/src/copy.cpp"
    "${AMRNB_COMMON_DIR}/src/div_32.cpp"
    "${AMRNB_COMMON_DIR}/src/l_abs.cpp"
    "${AMRNB_COMMON_DIR}/src/r_fft.cpp"
    "${AMRNB_COMMON_DIR}/src/vad1.cpp"
    "${AMRNB_COMMON_DIR}/src/vad2.cpp"
)

add_library(opencore-amrnb STATIC
    "${OPENCORE_AMR_ROOT}/amrnb/wrapper.cpp"
    ${AMRNB_DEC_SOURCES}
    ${AMRNB_ENC_SOURCES}
    ${AMRNB_COMMON_SOURCES}
)

target_include_directories(opencore-amrnb PUBLIC
    "${OPENCORE_AMR_ROOT}/amrnb" # for interf_enc.h, interf_dec.h
    "${OSCL_DIR}"
    "${AMRNB_DEC_DIR}/include"
    "${AMRNB_COMMON_DIR}/include"
    "${AMR_BASE}/common/dec/include"
)

target_include_directories(opencore-amrnb PRIVATE
    "${AMRNB_DEC_DIR}/src"
    "${AMRNB_ENC_DIR}/src"
)

# Keep opencore's arithmetic ABI separate from the EVS reference implementation.
# Some identically named routines carry an extra overflow-pointer parameter.
# L_mac's C-equivalent header uses non-static __inline. MSVC emits its
# out-of-line COMDAT for VAD2; EVS exports a different three-argument L_mac.
# Namespace even header-defined arithmetic, never suppress duplicate symbols.
foreach(_symbol L_mac L_abs L_negate L_shr_r div_s mult_r negate norm_l norm_s
        shr shr_r sub Pow2)
    target_compile_definitions(opencore-amrnb PRIVATE "${_symbol}=telephony_amrnb_${_symbol}")
endforeach()

if(MSVC)
    target_compile_definitions(opencore-amrnb PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(opencore-amrnb PRIVATE /wd4244 /wd4267 /wd4305 /wd4018)
endif()

# ---------------------------------------------------------------------------
# opencore-amrwb
# ---------------------------------------------------------------------------

set(AMRWB_DEC_DIR "${AMR_BASE}/amr_wb/dec")

file(GLOB AMRWB_DEC_SOURCES "${AMRWB_DEC_DIR}/src/*.cpp")

# Exclude files as per Makefile.am
list(REMOVE_ITEM AMRWB_DEC_SOURCES
    "${AMRWB_DEC_DIR}/src/decoder_amr_wb.cpp"
)

add_library(opencore-amrwb STATIC
    "${OPENCORE_AMR_ROOT}/amrwb/wrapper.cpp"
    ${AMRWB_DEC_SOURCES}
)

# EVS exports float codebooks with these names; opencore uses int16 tables.
# Never let /FORCE:MULTIPLE (or archive order) choose one for both codecs.
target_compile_definitions(opencore-amrwb PRIVATE
    dico1_isf=telephony_amrwb_dico1_isf
    dico2_isf=telephony_amrwb_dico2_isf
    dico21_isf_36b=telephony_amrwb_dico21_isf_36b
    dico22_isf_36b=telephony_amrwb_dico22_isf_36b
    dico23_isf_36b=telephony_amrwb_dico23_isf_36b
    phase_dispersion=telephony_amrwb_phase_dispersion
    t_qua_gain6b=telephony_amrwb_t_qua_gain6b
    t_qua_gain7b=telephony_amrwb_t_qua_gain7b
)

target_include_directories(opencore-amrwb PUBLIC
    "${OPENCORE_AMR_ROOT}/amrwb" # for dec_if.h
    "${OSCL_DIR}"
    "${AMRWB_DEC_DIR}/include"
    "${AMR_BASE}/common/dec/include"
)

target_include_directories(opencore-amrwb PRIVATE
    "${AMRWB_DEC_DIR}/src"
)

if(MSVC)
    target_compile_definitions(opencore-amrwb PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(opencore-amrwb PRIVATE /wd4244 /wd4267 /wd4305 /wd4018)
endif()
