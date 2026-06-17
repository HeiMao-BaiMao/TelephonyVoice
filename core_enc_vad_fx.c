/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Provides the 19 encoder-side core / VAD / preprocessing symbols that the
 *  vendored fixed-point snapshot references but does not implement:
 *
 *      enc_acelp_tcx_main
 *      core_encode_update
 *      init_coder_ace_plus
 *      MDCT_selector_reset
 *      InitTransientDetection
 *      enc_prm_rf
 *      SetModeIndex
 *      analysisCldfbEncoder_fx
 *      MDCT_selector
 *      long_enr_fx
 *      find_uv_fx
 *      signal_clas_fx
 *      core_acelp_tcx20_switching
 *      analy_sp
 *      AdjustFirstSID
 *      RunTransientDetection
 *      GetTCXAvgTemporalFlatnessMeasure
 *      SetTCXModeInfo
 *      vad_proc
 *
 *  Why this parent-side helper exists
 *  ----------------------------------
 *  The upstream float implementations live under
 *      external/3gpp-evs/lib_enc/*.c
 *  and in external/3gpp-evs/lib_com/cldfb.c.  They use float Encoder_State /
 *  float arrays and pull in the float prot.h chain, so they cannot be linked
 *  into the FX static libraries.  The vendored fixed-point snapshot does not
 *  ship FX implementations for these symbols either, which leaves the FX link
 *  with 47 unresolved external references (19 of them are the symbols above).
 *
 *  Because external/3gpp-evs/ is read-only in this repo, the helpers are
 *  provided here and are intended to be attached to evs-lib-com-fx via
 *  EVS_FX_EXTRAS_LIB_COM in cmake/3gpp-evs.cmake.
 *
 *  Porting strategy
 *  ----------------
 *  The primary goal is to make the FX encoder link and run without crashing.
 *  Bit-exact fidelity to the missing upstream FX reference is explicitly a
 *  later refinement goal.  Where the fixed-point port is straightforward
 *  (state init, simple energy updates, output zeroing) a minimal functional
 *  implementation is provided.  Where the algorithm is large and not required
 *  for basic link-unblock (ACELP/TCX core encoding, RF parameter packing,
 *  MDCT switching heuristics) the symbol is a stub that returns sensible
 *  defaults and is marked with a TODO comment.
 *
 *  All functions match the fixed-point prototypes in
 *      external/3gpp-evs/lib_com/prot_fx.h
 * ============================================================================ */

#include "typedefs.h"
#include "prot_fx.h"

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

/* ----------------------------------------------------------------------------
 *  enc_acelp_tcx_main
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/enc_acelp_tcx_main.c
 *
 *  TODO: link-unblock stub.  The real ACELP/TCX core encoder is not
 *        implemented here; the function merely records the coder type and
 *        writes safe defaults into its output arguments.
 * ---------------------------------------------------------------------------- */
void enc_acelp_tcx_main(
    const Word16 new_samples[],
    Encoder_State_fx *st,
    const Word16 coder_type,
    const Word16 pitch[3],
    const Word16 voicing[3],
    Word16 Aw[NB_SUBFR16k * (M + 1)],
    const Word16 lsp_new[M],
    const Word16 lsp_mid[M],
    HANDLE_FD_CNG_ENC hFdCngEnc,
    Word32 bwe_exc_extended[],
    Word16 *voice_factors,
    Word16 pitch_buf[],
    Word16 vad_hover_flag,
    const Word16 vad_flag_dtx,
    Word16 *Q_new,
    Word16 *shift
)
{
    Word16 i;

    UNUSED(new_samples);
    UNUSED(pitch);
    UNUSED(voicing);
    UNUSED(Aw);
    UNUSED(lsp_new);
    UNUSED(lsp_mid);
    UNUSED(hFdCngEnc);
    UNUSED(bwe_exc_extended);
    UNUSED(vad_hover_flag);
    UNUSED(vad_flag_dtx);
    UNUSED(Q_new);
    UNUSED(shift);

    st->last_coder_type_fx = coder_type;

    for (i = 0; i < NB_SUBFR16k; i++)
    {
        voice_factors[i] = 0;
        pitch_buf[i]     = PIT_MIN;
    }
}

/* ----------------------------------------------------------------------------
 *  core_encode_update
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/core_enc_updt.c
 *
 *  TODO: link-unblock stub.  The real buffer update is not implemented.
 * ---------------------------------------------------------------------------- */
void core_encode_update(Encoder_State_fx *st)
{
    UNUSED(st);
}

/* ----------------------------------------------------------------------------
 *  init_coder_ace_plus
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/core_enc_init.c
 *
 *  Minimal functional implementation: sets the core framing / sampling-rate
 *  fields that downstream FX code expects to be initialized here.
 * ---------------------------------------------------------------------------- */
void init_coder_ace_plus(Encoder_State_fx *st, const Word16 shift)
{
    Word16 L_subfr;

    UNUSED(shift);

    st->tcxonly = getTcxonly(st->total_brate_fx);

    st->sr_core  = getCoreSamplerateMode2(st->total_brate_fx, st->bwidth_fx, st->rf_mode);
    st->fscale   = sr2fscale(st->sr_core);
    st->narrowBand = (st->bwidth_fx == NB) ? 1 : 0;

    st->L_frame_fx      = (Word16)(st->sr_core / 50);
    st->L_frame_past = -1;
    st->L_frameTCX   = (Word16)(st->input_Fs_fx / 50);

    if (st->L_frame_fx == L_FRAME16k && st->total_brate_fx <= ACELP_32k)
    {
        st->nb_subfr = NB_SUBFR16k;
    }
    else
    {
        st->nb_subfr = NB_SUBFR;
    }
    L_subfr = (Word16)(st->L_frame_fx / st->nb_subfr);
    UNUSED(L_subfr);

    st->encoderLookahead_enc = (Word16)NS2SA(st->sr_core, ACELP_LOOK_NS);
    st->encoderLookahead_FB  = (Word16)NS2SA(st->input_Fs_fx, ACELP_LOOK_NS);

    if (st->sr_core <= 16000 && st->tcxonly == 0)
    {
        st->lpcQuantization = 1;
    }
    else
    {
        st->lpcQuantization = 0;
    }

    st->next_force_safety_net_fx = 0;
    st->prev_coder_type_fx = GENERIC;
    st->currEnergyHF_fx    = 0;
    st->currEnergyHF_e_fx  = 0;
}

/* ----------------------------------------------------------------------------
 *  MDCT_selector_reset
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/mdct_selector.c
 *
 *  Minimal functional implementation: resets the MDCT selector history.
 * ---------------------------------------------------------------------------- */
void MDCT_selector_reset(Encoder_State_fx *st)
{
    st->prev_hi_ener   = 0;
    st->prev_hi_sparse = -1;
}

/* ----------------------------------------------------------------------------
 *  InitTransientDetection
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/transient_detection.c
 *
 *  Minimal functional implementation: zero the whole TransientDetection
 *  structure so that downstream readers see a deterministic idle state.
 * ---------------------------------------------------------------------------- */
void InitTransientDetection(
    Word16 nFrameLength,
    Word16 nTCXDelay,
    struct TransientDetection *pTransientDetection
)
{
    UNUSED(nFrameLength);
    UNUSED(nTCXDelay);

    set16_fx((Word16 *)pTransientDetection, 0,
             (Word16)(sizeof(*pTransientDetection) / sizeof(Word16)));
}

/* ----------------------------------------------------------------------------
 *  enc_prm_rf
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/enc_prm.c
 *
 *  TODO: link-unblock stub.  RF partial-copy bitstream packing is not
 *        implemented.
 * ---------------------------------------------------------------------------- */
void enc_prm_rf(
    Encoder_State_fx *st,
    const Word16 rf_frame_type,
    const Word16 fec_offset
)
{
    UNUSED(st);
    UNUSED(rf_frame_type);
    UNUSED(fec_offset);
}

/* ----------------------------------------------------------------------------
 *  SetModeIndex
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/setmodeindex.c
 *
 *  TODO: link-unblock stub.  The real core-coder mode switch is not
 *        reimplemented here.
 * ---------------------------------------------------------------------------- */
void SetModeIndex(
    Encoder_State_fx *st,
    Word32 total_brate,
    Word16 bwidth,
    const Word16 shift
)
{
    UNUSED(st);
    UNUSED(total_brate);
    UNUSED(bwidth);
    UNUSED(shift);
}

/* ----------------------------------------------------------------------------
 *  analysisCldfbEncoder_fx
 *
 *  Upstream float reference: external/3gpp-evs/lib_com/cldfb.c (analysisCldfbEncoder)
 *
 *  Minimal functional implementation: clears the output CLDFB buffers and
 *  reports neutral scale values so that downstream VAD / pre-processing does
 *  not operate on uninitialized data.
 * ---------------------------------------------------------------------------- */
void analysisCldfbEncoder_fx(
    Encoder_State_fx *st_fx,
    const Word16 *timeIn,
    Word32 realBuffer[CLDFB_NO_COL_MAX][CLDFB_NO_CHANNELS_MAX],
    Word32 imagBuffer[CLDFB_NO_COL_MAX][CLDFB_NO_CHANNELS_MAX],
    Word16 realBuffer16[CLDFB_NO_COL_MAX][CLDFB_NO_CHANNELS_MAX],
    Word16 imagBuffer16[CLDFB_NO_COL_MAX][CLDFB_NO_CHANNELS_MAX],
    Word32 enerBuffSum[CLDFB_NO_CHANNELS_MAX],
    Word16 *enerBuffSum_exp,
    CLDFB_SCALE_FACTOR *scale
)
{
    Word32 flatLen32;
    Word16 flatLen16;

    UNUSED(st_fx);
    UNUSED(timeIn);

    flatLen32 = (Word32)(CLDFB_NO_COL_MAX * CLDFB_NO_CHANNELS_MAX);
    flatLen16 = (Word16)flatLen32;

    set32_fx(&realBuffer[0][0], 0, flatLen16);
    set32_fx(&imagBuffer[0][0], 0, flatLen16);
    set16_fx(&realBuffer16[0][0], 0, flatLen16);
    set16_fx(&imagBuffer16[0][0], 0, flatLen16);
    set32_fx(enerBuffSum, 0, CLDFB_NO_CHANNELS_MAX);

    *enerBuffSum_exp = 0;
    scale->lb_scale  = 0;
    scale->hb_scale  = 0;
}

/* ----------------------------------------------------------------------------
 *  MDCT_selector
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/mdct_selector.c
 *
 *  TODO: link-unblock stub.  The MDCT/ACELP switching heuristic is not
 *        implemented; the existing st->core_fx decision is left unchanged.
 * ---------------------------------------------------------------------------- */
void MDCT_selector(
    Encoder_State_fx *st,
    Word16 sp_floor,
    Word16 Etot,
    Word16 cor_map_sum,
    const Word16 voicing[],
    const Word32 enerBuffer[],
    Word16 enerBuffer_exp,
    Word16 vadflag
)
{
    UNUSED(st);
    UNUSED(sp_floor);
    UNUSED(Etot);
    UNUSED(cor_map_sum);
    UNUSED(voicing);
    UNUSED(enerBuffer);
    UNUSED(enerBuffer_exp);
    UNUSED(vadflag);
}

/* ----------------------------------------------------------------------------
 *  long_enr_fx
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/long_enr.c
 *
 *  Minimal functional implementation: handles the initial-frames bootstrap of
 *  the long-term speech / noise energy estimates.  The steady-state IIR
 *  smoothing is left as a TODO.
 * ---------------------------------------------------------------------------- */
void long_enr_fx(
    Encoder_State_fx *st_fx,
    const Word16 Etot,
    const Word16 localVAD_HE_SAD,
    Word16 high_lpn_flag
)
{
    Word16 tmp;

    UNUSED(localVAD_HE_SAD);
    UNUSED(Etot);
    UNUSED(high_lpn_flag);

    if (st_fx->ini_frame_fx < 4)
    {
        st_fx->lp_noise_fx = st_fx->totalNoise_fx;
        tmp = (Word16)(st_fx->lp_noise_fx + 10);
        if (st_fx->lp_speech_fx < tmp)
        {
            st_fx->lp_speech_fx = tmp;
        }
    }
}

/* ----------------------------------------------------------------------------
 *  find_uv_fx
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/find_uv.c
 *
 *  Minimal functional implementation: returns a safe generic/unvoiced coder
 *  type and clears the short-pitch flag.
 * ---------------------------------------------------------------------------- */
Word16 find_uv_fx(
    Encoder_State_fx *st_fx,
    const Word16 *T_op_fr,
    const Word16 *voicing_fr,
    const Word16 *voicing,
    const Word16 *speech,
    const Word16 localVAD,
    const Word32 *ee,
    const Word16 corr_shift,
    const Word16 relE,
    const Word16 Etot,
    const Word32 hp_E[],
    const Word16 Q_new,
    Word16 *flag_spitch,
    const Word16 voicing_sm,
    const Word16 shift,
    const Word16 last_core_orig
)
{
    UNUSED(st_fx);
    UNUSED(T_op_fr);
    UNUSED(voicing_fr);
    UNUSED(voicing);
    UNUSED(speech);
    UNUSED(ee);
    UNUSED(corr_shift);
    UNUSED(relE);
    UNUSED(Etot);
    UNUSED(hp_E);
    UNUSED(Q_new);
    UNUSED(voicing_sm);
    UNUSED(shift);
    UNUSED(last_core_orig);

    *flag_spitch = 0;

    if (localVAD == 0)
    {
        return UNVOICED;
    }

    return GENERIC;
}

/* ----------------------------------------------------------------------------
 *  signal_clas_fx
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/sig_clas.c
 *
 *  Minimal functional implementation: returns a voiced/unvoiced FEC
 *  classification based only on the local VAD and the current coder type.
 * ---------------------------------------------------------------------------- */
Word16 signal_clas_fx(
    Encoder_State_fx *st,
    Word16 *coder_type,
    const Word16 voicing[3],
    const Word16 *speech,
    const Word16 localVAD,
    const Word16 pit[3],
    const Word32 *ee,
    const Word16 relE,
    const Word16 L_look,
    Word16 *uc_clas
)
{
    UNUSED(st);
    UNUSED(voicing);
    UNUSED(speech);
    UNUSED(pit);
    UNUSED(ee);
    UNUSED(relE);
    UNUSED(L_look);

    *uc_clas = 0;

    if (localVAD == 0 || *coder_type == UNVOICED)
    {
        return UNVOICED_CLAS;
    }

    return VOICED_CLAS;
}

/* ----------------------------------------------------------------------------
 *  core_acelp_tcx20_switching
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/core_enc_ol.c
 *
 *  TODO: link-unblock stub.  The open-loop ACELP/TCX20 decision is not
 *        implemented.
 * ---------------------------------------------------------------------------- */
void core_acelp_tcx20_switching(
    Encoder_State_fx *st,
    const Word16 vad_flag,
    Word16 sp_aud_decision0,
    Word16 non_staX,
    Word16 *pitch,
    Word16 *pitch_fr,
    Word16 *voicing_fr,
    const Word16 currFlatness,
    const Word16 lsp_mid[M],
    const Word16 stab_fac,
    Word16 Q_new,
    Word16 shift
)
{
    UNUSED(st);
    UNUSED(vad_flag);
    UNUSED(sp_aud_decision0);
    UNUSED(non_staX);
    UNUSED(pitch);
    UNUSED(pitch_fr);
    UNUSED(voicing_fr);
    UNUSED(currFlatness);
    UNUSED(lsp_mid);
    UNUSED(stab_fac);
    UNUSED(Q_new);
    UNUSED(shift);
}

/* ----------------------------------------------------------------------------
 *  analy_sp
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/analy_sp.c
 *
 *  Minimal functional implementation: clears all output analysis buffers and
 *  reports zero total energy.
 * ---------------------------------------------------------------------------- */
void analy_sp(
    Word16 *speech,
    const Word16 Q_new,
    Word32 *fr_bands,
    Word32 *lf_E,
    Word16 *Etot,
    const Word16 min_band,
    const Word16 max_band,
    const Word32 e_min_scaled,
    Word16 Scale_fac[2],
    Word32 *Bin_E,
    Word32 *Bin_E_old,
    Word32 *PS,
    Word16 *EspecdB,
    Word32 *band_energies,
    Word16 *fft_buff
)
{
    UNUSED(speech);
    UNUSED(Q_new);
    UNUSED(min_band);
    UNUSED(max_band);
    UNUSED(e_min_scaled);

    set32_fx(fr_bands,       0, (Word16)(2 * NB_BANDS));
    set32_fx(lf_E,           0, (Word16)(2 * VOIC_BINS));
    set32_fx(Bin_E,          0, L_FFT);
    set32_fx(Bin_E_old,      0, (Word16)(L_FFT / 2));
    set32_fx(PS,             0, (Word16)(L_FFT / 2));
    set16_fx(EspecdB,        0, (Word16)(L_FFT / 2));
    set32_fx(band_energies,  0, (Word16)(2 * NB_BANDS));
    set16_fx(fft_buff,       0, (Word16)(2 * L_FFT));

    Scale_fac[0] = 0;
    Scale_fac[1] = 0;
    *Etot = 0;
}

/* ----------------------------------------------------------------------------
 *  AdjustFirstSID
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/nois_est.c
 *  (and the prototype in prot_fx.h)
 *
 *  TODO: link-unblock stub.  Returns 0 (no adjustment applied).
 * ---------------------------------------------------------------------------- */
Word16 AdjustFirstSID(
    Word16  npart,
    Word32 *msPeriodog,
    Word16  msPeriodog_exp,
    Word32 *energy_ho,
    Word16 *energy_ho_exp,
    Word32 *msNoiseEst,
    Word16 *msNoiseEst_exp,
    Word32 *msNoiseEst_old,
    Word16 *msNoiseEst_old_exp,
    Word16 *active_frame_counter,
    Encoder_State_fx *stcod
)
{
    UNUSED(npart);
    UNUSED(msPeriodog);
    UNUSED(msPeriodog_exp);
    UNUSED(energy_ho);
    UNUSED(energy_ho_exp);
    UNUSED(msNoiseEst);
    UNUSED(msNoiseEst_exp);
    UNUSED(msNoiseEst_old);
    UNUSED(msNoiseEst_old_exp);
    UNUSED(active_frame_counter);
    UNUSED(stcod);

    return 0;
}

/* ----------------------------------------------------------------------------
 *  RunTransientDetection
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/transient_detection.c
 *
 *  TODO: link-unblock stub.  The real time-domain transient detector is not
 *        implemented.
 * ---------------------------------------------------------------------------- */
void RunTransientDetection(
    Word16 const *input,
    Word16 nSamplesAvailable,
    struct TransientDetection *pTransientDetection
)
{
    UNUSED(input);
    UNUSED(nSamplesAvailable);
    UNUSED(pTransientDetection);
}

/* ----------------------------------------------------------------------------
 *  GetTCXAvgTemporalFlatnessMeasure
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/transient_detection.c
 *
 *  Minimal functional implementation: returns 0 (maximally flat), keeping the
 *  link happy without running the real subblock-energy accumulation.
 * ---------------------------------------------------------------------------- */
Word16 GetTCXAvgTemporalFlatnessMeasure(
    struct TransientDetection const *pTransientDetection,
    Word16 nCurrentSubblocks,
    Word16 nPrevSubblocks
)
{
    UNUSED(pTransientDetection);
    UNUSED(nCurrentSubblocks);
    UNUSED(nPrevSubblocks);

    return 0;
}

/* ----------------------------------------------------------------------------
 *  SetTCXModeInfo
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/transient_detection.c
 *
 *  Minimal functional implementation: chooses a deterministic TCX window
 *  configuration based on the enable flags in the encoder state.
 * ---------------------------------------------------------------------------- */
void SetTCXModeInfo(
    Encoder_State_fx *st,
    struct TransientDetection const *pTransientDetection,
    Word16 *tcxModeOverlap
)
{
    UNUSED(pTransientDetection);

    if (st->codec_mode != MODE2)
    {
        return;
    }

    if (st->tcx10Enabled && st->tcx20Enabled)
    {
        st->tcxMode = TCX_20;
    }
    else if (st->tcx10Enabled)
    {
        st->tcxMode = TCX_10;
    }
    else if (st->tcx20Enabled)
    {
        st->tcxMode = TCX_20;
    }
    else
    {
        st->tcxMode = NO_TCX;
    }

    if (st->last_core_fx == ACELP_CORE || st->last_core_fx == AMR_WB_CORE)
    {
        st->tcx_cfg.tcx_last_overlap_mode = TRANSITION_OVERLAP;
    }
    else
    {
        st->tcx_cfg.tcx_last_overlap_mode = st->tcx_cfg.tcx_curr_overlap_mode;
    }

    *tcxModeOverlap = FULL_OVERLAP;
}

/* ----------------------------------------------------------------------------
 *  vad_proc
 *
 *  Upstream float reference: external/3gpp-evs/lib_enc/vad_proc.c
 *
 *  Minimal functional implementation: passes the input VAD decision through and
 *  clears the CLDFB hangover adjustment.
 * ---------------------------------------------------------------------------- */
Word16 vad_proc(
    T_CldfbVadState *vad_st,
    Word32 realBuffer[CLDFB_NO_COL_MAX][CLDFB_NO_CHANNELS_MAX],
    Word32 imagBuffer[CLDFB_NO_COL_MAX][CLDFB_NO_CHANNELS_MAX],
    Word16 riBuffer_exp,
    Word16 *cldfb_addition,
    Word32 enerBuffer[CLDFB_NO_CHANNELS_MAX],
    Word16 enerBuffer_exp,
    Word16 bandwidth,
    Word16 vada_flag
)
{
    UNUSED(vad_st);
    UNUSED(realBuffer);
    UNUSED(imagBuffer);
    UNUSED(riBuffer_exp);
    UNUSED(enerBuffer);
    UNUSED(enerBuffer_exp);
    UNUSED(bandwidth);

    *cldfb_addition = 0;

    return vada_flag;
}
