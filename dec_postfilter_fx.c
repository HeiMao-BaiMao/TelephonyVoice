/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Provides the decoder post-filter / concealment / ACELP de-indexing symbols
 *  declared in external/3gpp-evs/lib_com/prot_fx.h:
 *
 *      Init_post_filter
 *      nb_post_filt
 *      formant_post_filt
 *      scale_st
 *      blend_subfr2
 *      decod_unvoiced_fx
 *      dec_acelp_tcx_frame
 *      waveform_adj2_fix
 *      concealment_update2_x
 *      tcx_ltp_post
 *      post_decoder
 *      open_decoder_LPD
 *      PulseResynchronization
 *      D_ACELP_indexing
 *
 *  Why this helper exists
 *  ----------------------
 *  The upstream float implementations live in
 *      external/3gpp-evs/lib_dec/dec_post.c
 *      external/3gpp-evs/lib_dec/post_dec.c
 *      external/3gpp-evs/lib_dec/dec_acelp.c
 *      external/3gpp-evs/lib_dec/dec_acelp_tcx_main.c
 *      external/3gpp-evs/lib_dec/dec_uv.c
 *      external/3gpp-evs/lib_dec/waveadjust_fec_dec.c
 *      external/3gpp-evs/lib_dec/er_sync_exc.c
 *      external/3gpp-evs/lib_dec/core_dec_init.c
 *      external/3gpp-evs/lib_com/tcx_ltp.c
 *      external/3gpp-evs/lib_com/calc_st_com.c
 *  and have float/int ABIs that cannot be linked into the FX static libraries.
 *  The vendored fixed-point snapshot does not ship FX counterparts for these
 *  particular symbols, so the FX decoder cannot link.  Because
 *  external/3gpp-evs/ is read-only, the helpers live here and are intended to
 *  be attached to evs-lib-com-fx via EVS_FX_EXTRAS_LIB_COM.
 *
 *  Numerical contract
 *  ------------------
 *  The implementations below are primarily "link-unblock" stubs/approximations.
 *  The simple helpers (Init_post_filter, scale_st, blend_subfr2,
 *  waveform_adj2_fix) are ported to fixed-point; the larger decoder glue
 *  functions (nb_post_filt, formant_post_filt, post_decoder, open_decoder_LPD,
 *  dec_acelp_tcx_frame, decod_unvoiced_fx, tcx_ltp_post, PulseResynchronization,
 *  D_ACELP_indexing, concealment_update2_x) are stubbed with sensible defaults
 *  and marked with TODO comments.  The goal is to make the symbols link so that
 *  the FX decoder can be exercised; bit-exact fidelity is a later refinement.
 *
 *  Implementation notes
 *  --------------------
 *  The FX typedef shim force-includes the Word16/Word32 environment, FX
 *  constants (cnst_fx.h) and the FX-native Decoder_State_fx / T_PLCInfo /
 *  PFSTAT layouts.  We still need prot_fx.h here because this helper accesses
 *  struct fields and calls a few safe FX configuration helpers
 *  (getCoreSamplerateMode2, sr2fscale, initPitchLagParameters, getTcxonly,
 *  getTcxBandwidth, getTcxLtp, getNumTcxCodedLines, getTnsAllowed, getCtxHm,
 *  getResq, getTcxLpcShapedAri).
 * ============================================================================ */

#include "typedefs.h"
#include "prot_fx.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 *  Small inline helpers
 * --------------------------------------------------------------------------- */

static Word16 evs_abs16(Word16 x)
{
    return (Word16)(x < 0 ? -x : x);
}

static Word16 evs_min16(Word16 a, Word16 b)
{
    return (Word16)(a < b ? a : b);
}

static Word16 evs_max16(Word16 a, Word16 b)
{
    return (Word16)(a > b ? a : b);
}

/* ---------------------------------------------------------------------------
 *  Init_post_filter
 *  PFSTAT is the shared float-layout structure from stat_com.h; we keep the
 *  same layout but initialise it so that the first Word16 view of gain_prec
 *  is zero (float 0.0f has all-zero low 16 bits on little-endian hosts).
 * --------------------------------------------------------------------------- */
void Init_post_filter(PFSTAT *pfstat)
{
    Word16 i;

    pfstat->on = 0;
    pfstat->reset = 0;

    for (i = 0; i < L_SUBFR; i++)
    {
        pfstat->mem_pf_in[i] = 0.0f;
    }
    for (i = 0; i < DECMEM_RES2; i++)
    {
        pfstat->mem_res2[i] = 0.0f;
    }
    for (i = 0; i < L_SUBFR; i++)
    {
        pfstat->mem_stp[i] = 0.0f;
    }
    for (i = 0; i < M; i++)
    {
        pfstat->mem_zero[i] = 0.0f;
    }

    pfstat->gain_prec = 0.0f;   /* Word16 view == 0 on LE hosts */

    return;
}

/* ---------------------------------------------------------------------------
 *  scale_st - fixed-point AGC for the post-filter
 *  gain_prec is treated as Q15 (the FX callers pass &pfstat->gain_prec).
 * --------------------------------------------------------------------------- */
void scale_st(const Word16 *sig_in, Word16 *sig_out, Word16 *gain_prec, Word16 L_subfr)
{
    Word32 gain_in = 0;
    Word32 gain_out = 0;
    Word32 g0;
    Word32 gain;
    Word16 i;

    for (i = 0; i < L_subfr; i++)
    {
        gain_in += evs_abs16(sig_in[i]);
        gain_out += evs_abs16(sig_out[i]);
    }

    if (gain_in == 0)
    {
        g0 = 0;
    }
    else if (gain_out == 0)
    {
        *gain_prec = 0;
        return;
    }
    else
    {
        g0 = (gain_in * (Word32)AGC_FAC1_FX) / gain_out;
        if (g0 > 32767)
        {
            g0 = 32767;
        }
    }

    gain = *gain_prec;
    for (i = 0; i < L_subfr; i++)
    {
        gain = ((gain * (Word32)AGC_FAC_FX) + (g0 * (Word32)AGC_FAC1_FX)) >> 15;
        if (gain > 32767)
        {
            gain = 32767;
        }
        sig_out[i] = (Word16)(((gain * (Word32)sig_out[i]) + 0x4000) >> 15);
    }

    *gain_prec = (Word16)gain;
    return;
}

/* ---------------------------------------------------------------------------
 *  blend_subfr2 - fade-out / fade-in over the second half of a subframe
 * --------------------------------------------------------------------------- */
void blend_subfr2(Word16 *sigIn1, Word16 *sigIn2, Word16 *sigOut)
{
    Word16 i;
    Word16 half = L_SUBFR / 2;
    Word32 ratio;

    for (i = 0; i < half; i++)
    {
        ratio = ((Word32)i * 32768) / half;   /* Q15 */
        sigOut[i] = (Word16)((((Word32)(32767 - ratio) * sigIn1[i]) +
                               ((Word32)ratio * sigIn2[i]) + 0x4000) >> 15);
    }

    return;
}

/* ---------------------------------------------------------------------------
 *  nb_post_filt - narrowband post-filter (link-unblock stub)
 *  TODO: proper fixed-point port of Dec_postfilt / pst_ltp / modify_pst_param.
 * --------------------------------------------------------------------------- */
void nb_post_filt(
    const Word16 L_frame,
    PFSTAT *pfstat,
    Word16 *psf_lp_noise,
    const Word16 tmp_noise,
    Word16 *Synth,
    const Word16 *Aq,
    const Word16 *Pitch_buf,
    const Word16 coder_type,
    const Word16 BER_detect,
    const Word16 disable_hpf)
{
    Word16 i;

    (void)Aq;
    (void)Pitch_buf;
    (void)disable_hpf;

    if (BER_detect == 0 && coder_type == INACTIVE)
    {
        /* 0.95 / 0.05 in Q15 */
        *psf_lp_noise = (Word16)(((Word32)(*psf_lp_noise) * 31130 +
                                  (Word32)tmp_noise * 1638) >> 15);
    }

    if (pfstat->reset)
    {
        for (i = 0; i < DECMEM_RES2; i++)
        {
            pfstat->mem_res2[i] = 0.0f;
        }
        for (i = 0; i < L_SUBFR; i++)
        {
            pfstat->mem_pf_in[i] = 0.0f;
            pfstat->mem_stp[i] = 0.0f;
        }
        pfstat->gain_prec = 0.0f;
        pfstat->reset = 0;
    }
    else if ((Word16)L_frame > 0)
    {
        Word16 copy = evs_min16(L_SUBFR, L_frame);
        for (i = 0; i < copy; i++)
        {
            pfstat->mem_pf_in[i] = (float)Synth[L_frame - copy + i];
        }
    }

    return;
}

/* ---------------------------------------------------------------------------
 *  formant_post_filt - wideband formant post-filter (link-unblock stub)
 *  TODO: proper fixed-point port of Dec_formant_postfilt.
 * --------------------------------------------------------------------------- */
void formant_post_filt(
    PFSTAT *pfstat,
    Word16 *synth_in,
    Word16 *Aq,
    Word16 *synth_out,
    Word16 L_frame,
    Word32 lp_noise,
    Word32 rate,
    const Word16 off_flag)
{
    Word16 i;

    (void)Aq;
    (void)lp_noise;
    (void)rate;
    (void)off_flag;

    if (pfstat->reset)
    {
        pfstat->reset = 0;
        for (i = 0; i < L_SUBFR; i++)
        {
            pfstat->mem_pf_in[i] = 0.0f;
            pfstat->mem_stp[i] = 0.0f;
        }
        pfstat->gain_prec = 0.0f;
    }
    else if (L_frame > 0)
    {
        Word16 copy = evs_min16(L_SUBFR, L_frame);
        for (i = 0; i < copy; i++)
        {
            pfstat->mem_pf_in[i] = (float)synth_in[L_frame - copy + i];
        }
    }

    if (synth_out != synth_in && L_frame > 0)
    {
        memmove(synth_out, synth_in, (size_t)L_frame * sizeof(Word16));
    }

    return;
}

/* ---------------------------------------------------------------------------
 *  post_decoder - top-level decoder post-processing (link-unblock stub)
 *  TODO: proper fixed-point port including bass post-filter and NB/WB branch.
 * --------------------------------------------------------------------------- */
void post_decoder(
    Decoder_State_fx *st,
    Word16 coder_type,
    Word16 synth_buf[],
    Word16 pit_gain[],
    Word16 pitch[],
    Word16 signal_out[],
    Word16 *bpf_noise_buf)
{
    Word16 i;
    Word16 *synth = synth_buf + st->old_synth_len;

    (void)coder_type;
    (void)pit_gain;
    (void)pitch;
    (void)bpf_noise_buf;

    st->pfstat.on = 0;

    for (i = 0; i < st->L_frame_fx; i++)
    {
        signal_out[i] = synth[i];
    }

    return;
}

/* ---------------------------------------------------------------------------
 *  open_decoder_LPD - LPD decoder reconfiguration / init (link-unblock stub)
 *  Sets the core samplerate, frame sizes and a handful of state fields that
 *  are needed before the decoder can run.  The full upstream routine also
 *  initialises MDCT windows, TNS, CLDFB and comfort-noise modules; those
 *  paths are omitted here to avoid dependencies on float-only helpers.
 *  TODO: complete fixed-point port.
 * --------------------------------------------------------------------------- */
void open_decoder_LPD(
    Decoder_State_fx *st,
    Word32 bitrate,
    Word16 bandwidth)
{
    Word16 i, j;
    Word32 sr_core;
    Word16 fscaleFB;
    Word8 tcxonly;

    st->total_brate_fx = bitrate;
    st->fscale_old = st->fscale;

    sr_core = getCoreSamplerateMode2(bitrate, bandwidth, st->rf_flag);
    st->sr_core = sr_core;
    st->fscale = sr2fscale(sr_core);
    fscaleFB = sr2fscale(st->output_Fs_fx);

    st->L_frame_fx = (Word16)(sr_core / 50);
    st->L_frameTCX = (Word16)(st->output_Fs_fx / 50);

    if (st->ini_frame_fx == 0)
    {
        st->last_L_frame_fx = st->L_frame_past = st->L_frame_fx;
        st->L_frameTCX_past = st->L_frameTCX;
    }

    tcxonly = getTcxonly(bitrate);
    st->tcxonly = tcxonly;

    if ((st->L_frame_fx == L_FRAME16k && bitrate <= ACELP_32k) ||
        (tcxonly && (sr_core == 32000 || sr_core == 16000)))
    {
        st->nb_subfr = NB_SUBFR16k;
    }
    else
    {
        st->nb_subfr = NB_SUBFR;
    }

    /* bits_frame ~= L_frame * FSCALE_DENOM * bitrate / (fscale * 128 * 100) */
    {
        long long num = (long long)st->L_frame_fx * FSCALE_DENOM * bitrate;
        long long den = (long long)st->fscale * 128 * 100;
        st->bits_frame = (Word16)((num + den / 2) / den);
    }

    st->TcxBandwidth = getTcxBandwidth(bandwidth);
    st->narrowBand = (bandwidth == NB) ? 1 : 0;

    /* Pre-emphasis factor */
    if (st->fscale < (Word16)((16000L * FSCALE_DENOM) / 12800))
    {
        st->preemph_fac = PREEMPH_FAC;
    }
    else if (st->fscale < (Word16)((24000L * FSCALE_DENOM) / 12800))
    {
        st->preemph_fac = PREEMPH_FAC_16k;
    }
    else
    {
        st->preemph_fac = PREEMPH_FAC_SWB;
    }

    /* LPC weighting */
    if (sr_core == 16000)
    {
        st->gamma = GAMMA16k;
    }
    else
    {
        st->gamma = GAMMA1;
    }

    st->lpcQuantization = (sr_core <= 16000 && !tcxonly) ? 1 : 0;
    st->numlpc = tcxonly ? 2 : 1;

    /* Pitch lag limits */
    st->pit_res_max = initPitchLagParameters(sr_core,
                                              &st->pit_min,
                                              &st->pit_fr1,
                                              &st->pit_fr1b,
                                              &st->pit_fr2,
                                              &st->pit_max);
    if (st->ini_frame_fx == 0)
    {
        st->pit_res_max_past = st->pit_res_max;
    }
    st->pit_max_TCX = (Word16)((st->pit_max * st->output_Fs_fx) / sr_core);
    st->pit_min_TCX = (Word16)((st->pit_min * st->output_Fs_fx) / sr_core);

    /* TCX-LTP / TCX configuration */
    st->tcxltp = getTcxLtp(sr_core);
    st->tcx_cfg.tcx_coded_lines = getNumTcxCodedLines(bandwidth);
    st->tcx_cfg.fIsTNSAllowed = getTnsAllowed(bitrate, st->igf);
    st->tcx_cfg.ctx_hm = getCtxHm(bitrate, st->rf_flag);
    st->tcx_cfg.resq = getResq(bitrate);
    st->tcx_lpc_shaped_ari = getTcxLpcShapedAri(bitrate, bandwidth, st->rf_flag);
    st->tcx_cfg.sq_rounding = 0.375f;
    st->tcx_cfg.na_scale = 1.0f;
    st->tcx_cfg.bandwidth = (float)bandwidth;
    st->tcx_cfg.preemph_fac = st->preemph_fac / 32768.0f;

    /* Bass post-filter */
    st->bpf_gain_param = 0;
    for (i = 0; i < NBPSF_PIT_MAX; i++)
    {
        st->pst_old_syn_fx[i] = 0;
    }

    /* Synthesis / switching memories */
    st->old_synth_len = (Word16)(2 * st->L_frame_fx);
    st->old_synth_lenFB = (Word16)(2 * st->L_frameTCX);

    for (i = 0; i < NB_SUBFR16k * (M + 1); i++)
    {
        st->mem_Aq[i] = 0;
    }
    for (i = 0; i < L_SYN_MEM; i++)
    {
        st->mem_syn_r[i] = 0;
    }
    for (i = 0; i <= M; i++)
    {
        st->syn[i] = 0;
    }

    if (st->ini_frame_fx == 0)
    {
        for (i = 0; i < OLD_SYNTH_INTERNAL_DEC; i++)
        {
            st->old_synth[i] = 0;
        }
        for (i = 0; i < L_FRAME32k / 2; i++)
        {
            st->old_syn_Overl[i] = 0;
            st->syn_Overl_TDAC[i] = 0;
            st->syn_Overl[i] = 0;
        }
        for (i = 0; i < L_FRAME_MAX / 2; i++)
        {
            st->syn_OverlFB[i] = 0;
            st->syn_Overl_TDACFB[i] = 0;
        }
        st->last_is_cng = 0;
        st->con_tcx = 0;
        st->last_core_bfi = -1;
    }

    /* FEC / PLC defaults */
    st->prev_bfi_fx = 0;
    st->nbLostCmpt = 0;
    st->reset_mem_AR = 0;
    st->prev_widow_left_rect = 0;
    st->rate_switching_init = 1;
    st->voice_fac = -1;
    st->seed_tcx_plc = 21845;
    st->past_gpit = 0;
    st->past_gcode = 0;
    st->gc_threshold_fx = 0;
    st->lp_gainc_fx = 0;
    st->lp_gainp_fx = 0;
    st->lp_ener_fx = 0;

    for (i = 0; i < 2 * NB_SUBFR16k + 2; i++)
    {
        st->old_pitch_buf_fx[i] = (Word32)st->pit_min << 16;
        st->mem_pitch_gain[i] = 16384;    /* 1.0 in Q14 */
    }

    st->old_fpitch = (Word32)st->pit_min << 16;
    st->old_fpitchFB = (Word32)st->pit_max_TCX << 16;

    st->clas_dec = UNVOICED_CLAS;
    st->last_good_fx = UNVOICED_CLAS;

    /* Gain / VAD / flags */
    st->cummulative_damping_tcx = 32767;
    st->cummulative_damping = 32767;
    st->conceal_eof_gain = 16384;
    st->damping = 16384;
    st->gainHelper = 1;
    st->gainHelper_e = 0;
    st->stepCompensate = 0;
    st->stepCompensate_e = 0;
    st->cngTDLevel = 0;
    st->cngTDLevel_e = 0;
    st->old_gaintcx_bfi = 0;
    st->old_gaintcx_bfi_e = 0;
    st->last_gain_syn_deemph = 32767;
    st->last_gain_syn_deemph_e = 0;
    st->last_concealed_gain_syn_deemph = 32767;
    st->last_concealed_gain_syn_deemph_e = 0;
    st->plcBackgroundNoiseUpdated = 0;

    /* Coder state defaults */
    st->prev_coder_type_fx = GENERIC;
    st->cur_sub_Aq_fx[0] = 4096;   /* 1.0 in Q12, rest zero */
    for (i = 1; i <= M; i++)
    {
        st->cur_sub_Aq_fx[i] = 0;
    }
    st->prev_tilt_para_fx = 0;
    for (i = 0; i < LPC_SHB_ORDER - 2; i++)
    {
        st->prev_lsf_diff_fx[i] = 0;
    }

    st->dec_glr = (bitrate == ACELP_9k60 || bitrate == ACELP_16k40 || bitrate == ACELP_24k40) ? 1 : 0;
    st->dec_glr_idx = 0;
    st->enableGplc = 0;
    st->flagGuidedAcelp = 0;
    st->T0_4th = L_SUBFR;
    st->guidedT0 = L_SUBFR;

    st->tec_tfa = (bandwidth == SWB &&
                   (bitrate == ACELP_16k40 || bitrate == ACELP_24k40)) ? 1 : 0;
    st->tec_flag = 0;
    st->tfa_flag = 0;

    st->VAD = 0;
    st->flag_cna = 0;
    st->last_flag_cna = 0;
    st->enableTcxLpc = 1;
    st->envWeighted = 0;
    st->tcx_hm_LtpPitchLag = -1;

    st->core_ext_mode = 0;
    st->seed_acelp = 0;
    st->writeFECoffset = 0;
    st->prev_Q_exc_fr = 0;
    st->prev_Q_syn_fr = 0;

    st->scaleFactor.lb_scale = 0;
    st->scaleFactor.hb_scale = 0;

    if (st->tcxonly)
    {
        st->p_bpf_noise_buf = (Word16 *)0;
    }
    else
    {
        st->p_bpf_noise_buf = st->bpf_noise_buf;
    }

    /* CNG LPC shape to zero */
    for (i = 0; i < (NB_SUBFR16k + 1) * (M + 1); i++)
    {
        st->Aq_cng[i] = 0;
    }
    for (i = 0; i < M; i++)
    {
        st->mem_syn_unv_back[i] = 0;
        st->lsf_cng[i] = 0;
        st->lspold_cng[i] = 0;
        st->lsp_q_cng[i] = 0;
        st->old_lsp_q_cng[i] = 0;
        st->lsf_q_cng[i] = 0;
        st->old_lsf_q_cng[i] = 0;
    }

    /* Bass PF noise buffer */
    for (i = 0; i < L_FRAME_16k; i++)
    {
        st->bpf_noise_buf[i] = 0;
    }

    /* TCX LTP past values */
    if (st->ini_frame_fx == 0 || st->last_codec_mode == MODE1)
    {
        st->tcxltp_pitch_int = st->pit_max;
        st->tcxltp_pitch_fr = 0;
        st->tcxltp_last_gain_unmodified = 0;
        if (st->ini_frame_fx == 0)
        {
            for (i = 0; i < TCXLTP_MAX_DELAY; i++)
            {
                st->tcxltp_mem_in[i] = 0;
            }
            for (i = 0; i < L_FRAME48k; i++)
            {
                st->tcxltp_mem_out[i] = 0;
            }
            st->tcxltp_pitch_int_post_prev = 0;
            st->tcxltp_pitch_fr_post_prev = 0;
            st->tcxltp_gain_post_prev = 0;
            st->tcxltp_filt_idx_prev = -1;
        }
    }

    /* PLC waveadjust defaults */
    st->enablePlcWaveadjust = (bitrate >= HQ_48k) ? 1 : 0;
    st->tonalMDCTconceal.nScaleFactors = 0;
    st->tonalMDCTconceal.nSamples = 0;
    st->tonalMDCTconceal.lastPcmOut = (Word16 *)0;
    st->tonalMDCTconceal.lastBlockData.tonalConcealmentActive = 0;
    st->tonalMDCTconceal.lastBlockData.nSamples = 0;

    (void)fscaleFB;
    (void)j;

    return;
}

/* ---------------------------------------------------------------------------
 *  dec_acelp_tcx_frame - main ACELP/TCX frame decoder glue (link-unblock stub)
 *  TODO: full fixed-point port of decode_frame_type + decoder_LPD + side-info.
 * --------------------------------------------------------------------------- */
Word16 dec_acelp_tcx_frame(
    Decoder_State_fx *st,
    Word16 *coder_type,
    Word16 *concealWholeFrame,
    Word16 *pcmBuf,
    Word16 *bpf_noise_buf,
    Word16 *pcmbufFB,
    Word32 bwe_exc_extended[],
    Word16 *voice_factors,
    Word16 pitch_buf[])
{
    (void)st;
    (void)coder_type;
    (void)concealWholeFrame;
    (void)pcmBuf;
    (void)bpf_noise_buf;
    (void)pcmbufFB;
    (void)bwe_exc_extended;
    (void)voice_factors;
    (void)pitch_buf;

    return 0;
}

/* ---------------------------------------------------------------------------
 *  decod_unvoiced_fx - unvoiced excitation decoder (link-unblock stub)
 *  TODO: full fixed-point port using gaus_dec_fx / enhancer_fx.
 * --------------------------------------------------------------------------- */
void decod_unvoiced_fx(
    Decoder_State_fx *st_fx,
    const Word16 *Aq_fx,
    const Word16 coder_type_fx,
    Word16 *tmp_noise_fx,
    Word16 *pitch_buf_fx,
    Word16 *voice_factors_fx,
    Word16 *exc_fx,
    Word16 *exc2_fx,
    Word16 *bwe_exc_fx,
    Word16 *gain_buf)
{
    Word16 i;
    Word16 len = st_fx->L_frame_fx;

    (void)Aq_fx;
    (void)coder_type_fx;

    for (i = 0; i < len; i++)
    {
        exc_fx[i] = 0;
        exc2_fx[i] = 0;
        bwe_exc_fx[i] = 0;
    }

    for (i = 0; i < NB_SUBFR16k; i++)
    {
        pitch_buf_fx[i] = st_fx->pit_min;
        voice_factors_fx[i] = 0;
        gain_buf[i] = 0;
    }

    *tmp_noise_fx = 0;

    return;
}

/* ---------------------------------------------------------------------------
 *  waveform_adj2_fix - TD TCX waveform concealment adjustment
 *  Simplified fixed-point approximation: periodical extension from overlapbuf
 *  followed by a linear overlap-add with the current output.
 *  TODO: port add_noise and recovery-gain logic exactly.
 * --------------------------------------------------------------------------- */
void waveform_adj2_fix(
    Word16 *overlapbuf,
    Word16 *outx_new,
    Word16 *data_noise,
    Word16 *outx_new_n1,
    Word16 *nsapp_gain,
    Word16 *nsapp_gain_n,
    Word16 *recovery_gain,
    Word16 step_concealgain,
    Word16 pitch,
    Word16 Framesize,
    Word16 delay,
    Word16 bfi_cnt,
    Word16 bfi)
{
    Word16 i, n, len;
    Word16 sbuf[L_FRAME_MAX];
    Word32 ratio;

    (void)outx_new_n1;
    (void)nsapp_gain;
    (void)nsapp_gain_n;
    (void)delay;

    if (pitch <= 0)
    {
        return;
    }

    /* Periodical extension from the tail of overlapbuf */
    n = 0;
    while (n < Framesize)
    {
        Word16 copylen = evs_min16(pitch, (Word16)(Framesize - n));
        for (i = 0; i < copylen; i++)
        {
            sbuf[n + i] = overlapbuf[Framesize - pitch + i];
        }
        n += pitch;
    }
    for (i = 0; i < Framesize; i++)
    {
        overlapbuf[i] = sbuf[i];
    }

    /* Save current noisy IMDCT output for future noise generation */
    if (bfi)
    {
        for (i = 0; i < Framesize; i++)
        {
            data_noise[i] = outx_new[i];
        }
    }

    if (bfi_cnt == 4 || bfi == 0)
    {
        len = Framesize;
        if (len <= 0)
        {
            len = 1;
        }

        for (i = 0; i < Framesize; i++)
        {
            ratio = ((Word32)i * 32768) / len;
            outx_new[i] = (Word16)((((Word32)(32767 - ratio) * sbuf[i]) +
                                     ((Word32)ratio * outx_new[i]) + 0x4000) >> 15);
        }

        if (!bfi && recovery_gain != (Word16 *)0)
        {
            if (*recovery_gain > step_concealgain)
            {
                *recovery_gain -= step_concealgain;
            }
            else
            {
                *recovery_gain = 0;
            }
        }
    }
    else
    {
        for (i = 0; i < Framesize; i++)
        {
            outx_new[i] = sbuf[i];
        }
    }

    return;
}

/* ---------------------------------------------------------------------------
 *  concealment_update2_x - PLC state update after good frame (link-unblock stub)
 *  TODO: full fixed-point port of zero_pass / energy / log10 update.
 * --------------------------------------------------------------------------- */
void concealment_update2_x(Word16 *outx_new, void *_plcInfo, Word16 FrameSize)
{
    T_PLCInfo *plcInfo = (T_PLCInfo *)_plcInfo;
    Word16 i;
    Word16 prev = 0;
    Word16 zp = 0;
    Word32 ener = 0;

    for (i = 0; i < FrameSize; i++)
    {
        if ((prev >= 0 && outx_new[i] < 0) || (prev < 0 && outx_new[i] >= 0))
        {
            zp++;
        }
        prev = outx_new[i];
        ener += (Word32)outx_new[i] * (Word32)outx_new[i];
    }

    if (FrameSize > 0)
    {
        ener /= FrameSize;
    }

    plcInfo->zp_fx = zp;
    plcInfo->ener_fx = ener;

    return;
}

/* ---------------------------------------------------------------------------
 *  tcx_ltp_post - TCX LTP post-filter / resynchronisation (link-unblock stub)
 *  TODO: full fixed-point port of tcx_ltp_synth_filter family.
 * --------------------------------------------------------------------------- */
void tcx_ltp_post(
    Word8 tcxltp_on,
    Word16 core,
    Word16 L_frame,
    Word16 L_frame_core,
    Word16 delay,
    Word16 *sig,
    Word16 *tcx_buf,
    Word16 tcx_buf_len,
    Word16 bfi,
    Word16 pitch_int,
    Word16 pitch_fr,
    Word16 gain,
    Word16 *pitch_int_past,
    Word16 *pitch_fr_past,
    Word16 *gain_past,
    Word16 *filtIdx_past,
    Word16 pitres,
    Word16 *pitres_past,
    Word16 damping,
    Word16 SideInfoOnly,
    Word16 *mem_in,
    Word16 *mem_out,
    Word32 bitrate)
{
    Word16 i;

    (void)tcxltp_on;
    (void)core;
    (void)L_frame_core;
    (void)delay;
    (void)tcx_buf;
    (void)tcx_buf_len;
    (void)bfi;
    (void)pitch_fr;
    (void)gain;
    (void)damping;
    (void)SideInfoOnly;
    (void)mem_in;
    (void)bitrate;

    if (sig != (Word16 *)0 && mem_out != (Word16 *)0)
    {
        for (i = 0; i < L_frame; i++)
        {
            mem_out[i] = sig[i];
        }
    }

    if (pitch_int_past != (Word16 *)0)
    {
        *pitch_int_past = pitch_int;
    }
    if (pitch_fr_past != (Word16 *)0)
    {
        *pitch_fr_past = pitch_fr;
    }
    if (gain_past != (Word16 *)0)
    {
        *gain_past = 0;
    }
    if (filtIdx_past != (Word16 *)0)
    {
        *filtIdx_past = -1;
    }
    if (pitres_past != (Word16 *)0)
    {
        *pitres_past = pitres;
    }

    return;
}

/* ---------------------------------------------------------------------------
 *  PulseResynchronization - FEC pulse resynchronisation (link-unblock stub)
 *  TODO: full fixed-point port of the float algorithm in er_sync_exc.c.
 * --------------------------------------------------------------------------- */
void PulseResynchronization(
    Word16 const * const src_exc,
    Word16 * const dst_exc,
    Word16 const nFrameLength,
    Word16 const nSubframes,
    Word32 const pitchStart,
    Word32 const pitchEnd)
{
    (void)nSubframes;
    (void)pitchStart;
    (void)pitchEnd;

    if (src_exc != dst_exc && nFrameLength > 0)
    {
        memmove(dst_exc, src_exc, (size_t)nFrameLength * sizeof(Word16));
    }

    return;
}

/* ---------------------------------------------------------------------------
 *  D_ACELP_indexing - ACELP innovation de-indexing (link-unblock stub)
 *  TODO: full fixed-point port using pulsestostates / indx_fact tables.
 * --------------------------------------------------------------------------- */
void D_ACELP_indexing(
    Word16 code[],
    PulseConfig config,
    Word16 num_tracks,
    Word16 index[],
    Word16 *BER_detect)
{
    Word16 i;

    (void)config;
    (void)num_tracks;
    (void)index;

    for (i = 0; i < L_SUBFR; i++)
    {
        code[i] = 0;
    }

    *BER_detect = 0;

    return;
}
