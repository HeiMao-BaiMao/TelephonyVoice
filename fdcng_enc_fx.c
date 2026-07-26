/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Provides the FD-CNG encoder family symbols declared in
 *      external/3gpp-evs/lib_com/prot_fx.h (lines ~9425-9706)
 *  and implemented for the float reference in
 *      external/3gpp-evs/lib_enc/fd_cng_enc.c and
 *      external/3gpp-evs/lib_com/fd_cng_com.c.
 *
 *  The vendored fixed-point snapshot contains the FX prototypes and the
 *  FX ROM tables (FdCngSetup_*, sidparts_encoder_noise_est, scaleTable, ...),
 *  but it does not ship fixed-point implementations of these encoder-side
 *  FD-CNG functions.  Because external/3gpp-evs/ is read-only, the helpers
 *  live here and are intended to be attached to evs-lib-com-fx /
 *  evs-lib-enc-fx via EVS_FX_EXTRAS_LIB_COM.
 *
 *  Porting strategy
 *  ----------------
 *  The project-wide FX typedef shim redefines FD_CNG_COM with a small,
 *  FX-native layout that contains only the fields actually exercised by the
 *  existing decoder paths plus a minimal set of encoder scalars.  Several
 *  float-only fields (olapBufferAna, fftBuffer, cngNoiseLevel, sidNoiseEst,
 *  exc_cng, periodog, ...) are therefore not available, so the full
 *  STFT-based comfort-noise generator and the MSVQ SID encoder cannot be
 *  ported without extending the shim.  Those functions are implemented as
 *  minimal link-unblock stubs marked with TODO comments.
 *
 *  The functions that are straightforward to port with the current shim
 *  (create/init/delete/configure/reset, FdCng_exc, noisy_speech_detection)
 *  are implemented with reasonable fixed-point approximations.
 *
 *  Implementation notes
 *  --------------------
 *  "typedefs.h" and "prot_fx.h" are included for the FX types and the
 *  FD_CNG_ENC / FD_CNG_COM / Encoder_State_fx struct definitions; "stdlib.h"
 *  is included for calloc/free.  The FX typedef shim force-includes the
 *  Word16/Word32/basic-op environment.  All struct field accesses use the
 *  shim's FD_CNG_COM layout.
 * ============================================================================ */

#include "typedefs.h"
#include "prot_fx.h"
#include <stdlib.h>

/* -----------------------------------------------------------------------------
 * createFdCngEnc / deleteFdCngEnc
 * ---------------------------------------------------------------------------- */

void createFdCngEnc(HANDLE_FD_CNG_ENC *hFdCngEnc)
{
    HANDLE_FD_CNG_ENC hs;

    hs = (HANDLE_FD_CNG_ENC)calloc(1, sizeof(FD_CNG_ENC));
    if (hs != NULL)
    {
        hs->hFdCngCom = (HANDLE_FD_CNG_COM)calloc(1, sizeof(FD_CNG_COM));
        if (hs->hFdCngCom == NULL)
        {
            free(hs);
            hs = NULL;
        }
    }

    *hFdCngEnc = hs;
}

void deleteFdCngEnc(HANDLE_FD_CNG_ENC *hFdCngEnc)
{
    HANDLE_FD_CNG_ENC hsEnc = *hFdCngEnc;

    if (hsEnc != NULL)
    {
        if (hsEnc->hFdCngCom != NULL)
        {
            free(hsEnc->hFdCngCom);
            hsEnc->hFdCngCom = NULL;
        }
        free(hsEnc);
        *hFdCngEnc = NULL;
    }
}

/* -----------------------------------------------------------------------------
 * initFdCngEnc
 *
 * Mirrors the float initFdCngEnc() but only touches fields that exist in the
 * FX shim's FD_CNG_COM layout.  Partition tables that live in the (missing)
 * float-only FD_CNG_COM fields are not computed here; the encoder noise
 * estimator is currently a stub and does not need them.
 * ---------------------------------------------------------------------------- */

void initFdCngEnc(HANDLE_FD_CNG_ENC hsEnc, Word32 input_Fs, Word16 scale)
{
    HANDLE_FD_CNG_COM hsCom;
    Word16 regularStopBand;
    Word16 stopFFTbin;
    Word16 stopBand;
    Word16 nFFTpart;
    Word16 npart;
    Word16 fftlen;
    Word16 frameSize;
    Word16 i;

    (void)scale;

    if (hsEnc == NULL)
    {
        return;
    }

    hsCom = hsEnc->hFdCngCom;
    if (hsCom == NULL)
    {
        return;
    }

    /* Map common sampling rates to regularStopBand (input_Fs/800 capped at 40). */
    if (L_sub(input_Fs, 8000) == 0)
    {
        regularStopBand = 10;
    }
    else if (L_sub(input_Fs, 16000) == 0)
    {
        regularStopBand = 20;
    }
    else if (L_sub(input_Fs, 32000) == 0 || L_sub(input_Fs, 48000) == 0)
    {
        regularStopBand = 40;
    }
    else
    {
        regularStopBand = 40;
    }

    /* Choose FFT configuration. */
    if (regularStopBand == 10)
    {
        stopFFTbin = 160;
        stopBand = 160;
        nFFTpart = 17;
        fftlen = 512;
    }
    else
    {
        stopFFTbin = 256;
        stopBand = add(sub(regularStopBand, 16), stopFFTbin);
        nFFTpart = 20;
        fftlen = 512;
    }
    frameSize = shr(fftlen, 1);
    npart = nFFTpart;   /* Approximation: full partition init is stubbed. */

    /* Common FD_CNG state. */
    hsCom->numSlots = 16;
    hsCom->numCoreBands = 16;
    hsCom->regularStopBand = regularStopBand;
    hsCom->startBand = 2;
    hsCom->stopFFTbin = stopFFTbin;
    hsCom->stopBand = stopBand;
    hsCom->npart = npart;
    hsCom->frameSize = frameSize;
    hsCom->fftlen = fftlen;
    hsCom->CngBandwidth = 0;
    hsCom->CngBitrate = 0;
    hsCom->seed = 0x1234;
    hsCom->active_frame_counter = 0;
    hsCom->flag_noisy_speech = 0;
    hsCom->likelihood_noisy_speech = 0;
    hsCom->frame_type_previous = ACTIVE_FRAME;

    /* Flat LPC filter (A[0]=1 in Q12). */
    hsCom->A_cng[0] = 4096;
    for (i = 1; i < 17; i++)
    {
        hsCom->A_cng[i] = 0;
    }

    /* Clear overlap/time buffers. */
    set16_fx(hsCom->timeDomainBuffer, 0, L_FRAME16k);
    set16_fx(hsCom->olapBufferSynth, 0, FFTLEN);
    set16_fx(hsCom->olapBufferSynth2, 0, FFTLEN);

    /* Encoder-private MS buffers. */
    set32_fx(hsEnc->msPeriodog, 0, NPART);
    set32_fx(hsEnc->msBminWin, 0, NPART);
    set32_fx(hsEnc->msBminSubWin, 0, NPART);
    set16_fx(hsEnc->msPsd, 0, NPART);
    set32_fx(hsEnc->msAlpha, 0, NPART);
    set32_fx(hsEnc->msMinBuf, 0, (Word16)(MSNUMSUBFR * NPART));
    set32_fx(hsEnc->msCurrentMinOut, 0, NPART);
    set32_fx(hsEnc->msCurrentMin, 0, NPART);
    set32_fx(hsEnc->msCurrentMinSubWindow, 0, NPART);
    set16_fx(hsEnc->msLocalMinFlag, 0, NPART);
    set16_fx(hsEnc->msNewMinFlag, 0, NPART);
    set16_fx(hsEnc->msPsdFirstMoment, 0, NPART);
    set32_fx(hsEnc->msPsdSecondMoment, 0, NPART);
    set16_fx(hsEnc->msNoiseFloor, 0, NPART);
    set32_fx(hsEnc->msNoiseEst, 0, NPART);
    set32_fx(hsEnc->energy_ho, 0, NPART);
    set32_fx(hsEnc->msNoiseEst_old, 0, NPART);
    set16_fx(hsEnc->msPeriodogBuf, 0, (Word16)(MSBUFLEN * NPART));
    hsEnc->msPeriodogBufPtr = 0;
    set16_fx(hsEnc->msLogPeriodog, 0, NPART);
    set16_fx(hsEnc->msLogNoiseEst, 0, NPART);
    hsEnc->msPeriodog_exp = 0;
    hsEnc->msNoiseEst_exp = 0;
    hsEnc->energy_ho_exp = 0;
    hsEnc->msNoiseEst_old_exp = 0;

    /* Decoder-side shaping configuration defaults. */
    hsEnc->startBandDec = 2;
    hsEnc->stopBandDec = 160;
    hsEnc->stopFFTbinDec = 160;
    hsEnc->npartDec = 17;
    hsEnc->nFFTpartDec = 17;
    set16_fx(hsEnc->partDec, 0, NPART);
    set16_fx(hsEnc->midbandDec, 0, NPART);
}

/* -----------------------------------------------------------------------------
 * configureFdCngEnc
 *
 * Selects the appropriate FdCngSetup from the FX ROM tables and copies the
 * decoder-side partition information into FD_CNG_ENC.  The float-only window /
 * sine-table pointers are not stored because the shim FD_CNG_COM does not
 * carry those fields.
 * ---------------------------------------------------------------------------- */

void configureFdCngEnc(HANDLE_FD_CNG_ENC hsEnc, Word16 bandwidth, Word32 bitrate)
{
    HANDLE_FD_CNG_COM hsCom;
    const FD_CNG_SETUP *setup;
    Word16 i;
    Word16 lastPart;

    if (hsEnc == NULL || hsEnc->hFdCngCom == NULL)
    {
        return;
    }

    hsCom = hsEnc->hFdCngCom;

    /* Cap FB to SWB (matches float behaviour). */
    if (sub(bandwidth, FB) == 0)
    {
        bandwidth = SWB;
    }
    hsCom->CngBandwidth = bandwidth;
    hsCom->CngBitrate = bitrate;

    /* Select setup. */
    if (sub(bandwidth, NB) == 0)
    {
        setup = &FdCngSetup_nb;
    }
    else if (sub(bandwidth, WB) == 0)
    {
        if (L_sub(bitrate, ACELP_8k00) <= 0)
        {
            setup = &FdCngSetup_wb1;
        }
        else if (L_sub(bitrate, ACELP_13k20) <= 0)
        {
            setup = &FdCngSetup_wb2;
        }
        else
        {
            setup = &FdCngSetup_wb3;
        }
    }
    else
    {
        if (L_sub(bitrate, ACELP_13k20) <= 0)
        {
            setup = &FdCngSetup_swb1;
        }
        else
        {
            setup = &FdCngSetup_swb2;
        }
    }

    /* Copy setup (pointers to const FX ROM tables are shared safely). */
    hsCom->FdCngSetup = *setup;
    hsCom->fftlen = setup->fftlen;
    hsCom->frameSize = shr(setup->fftlen, 1);

    /* Decoder-side shaping partition info. */
    hsEnc->stopFFTbinDec = setup->stopFFTbin;
    hsEnc->startBandDec = hsCom->startBand;
    hsEnc->npartDec = setup->numPartitions;

    lastPart = setup->sidPartitions[sub(setup->numPartitions, 1)];
    hsEnc->stopBandDec = add(lastPart, 1);

    if (sub(hsEnc->stopFFTbinDec, 160) == 0)
    {
        hsEnc->nFFTpartDec = 17;
    }
    else if (sub(hsEnc->stopFFTbinDec, 256) == 0)
    {
        hsEnc->nFFTpartDec = 20;
    }
    else
    {
        hsEnc->nFFTpartDec = 21;
    }

    for (i = 0; i < setup->numPartitions && i < NPART; i++)
    {
        hsEnc->partDec[i] = setup->sidPartitions[i];
        /* Rough midband: average of previous upper boundary+1 and current. */
        if (i == 0)
        {
            hsEnc->midbandDec[i] = shr(setup->sidPartitions[i], 1);
        }
        else
        {
            hsEnc->midbandDec[i] = shr(add(add(setup->sidPartitions[i - 1], 1), setup->sidPartitions[i]), 1);
        }
    }
}

/* -----------------------------------------------------------------------------
 * resetFdCngEnc
 *
 * FX port of the float resetFdCngEnc() logic.  Detects rapid totalNoise
 * increases, bandwidth switches, and AMR-WB core switches and sets the reset
 * flag accordingly.
 * ---------------------------------------------------------------------------- */

void resetFdCngEnc(Encoder_State_fx *st)
{
    Word16 n;
    Word16 totalNoiseIncrease;
    Word32 sum;
    Word16 len;

    if (st == NULL)
    {
        return;
    }

    totalNoiseIncrease = sub(st->totalNoise_fx, st->last_totalNoise_fx);
    st->last_totalNoise_fx = st->totalNoise_fx;

    if (totalNoiseIncrease > 0)
    {
        if (sub(st->totalNoise_increase_len_fx, TOTALNOISE_HIST_SIZE) == 0)
        {
            for (n = 0; n < TOTALNOISE_HIST_SIZE - 1; n++)
            {
                st->totalNoise_increase_hist_fx[n] = st->totalNoise_increase_hist_fx[n + 1];
            }
            st->totalNoise_increase_hist_fx[TOTALNOISE_HIST_SIZE - 1] = totalNoiseIncrease;
        }
        else
        {
            st->totalNoise_increase_hist_fx[st->totalNoise_increase_len_fx] = totalNoiseIncrease;
            st->totalNoise_increase_len_fx = add(st->totalNoise_increase_len_fx, 1);
        }
    }
    else
    {
        st->totalNoise_increase_len_fx = 0;
    }

    sum = 0;
    len = st->totalNoise_increase_len_fx;
    for (n = 0; n < len; n++)
    {
        sum = L_add(sum, L_deposit_l(st->totalNoise_increase_hist_fx[n]));
    }
    totalNoiseIncrease = extract_l(L_shr(sum, 0)); /* Q8, same domain as float dB-ish */

    if ((totalNoiseIncrease > 5 && len == TOTALNOISE_HIST_SIZE && st->ini_frame_fx > 150) ||
        (sub(st->input_bwidth_fx, st->last_input_bwidth_fx) > 0) ||
        (sub(st->last_core_fx, AMR_WB_CORE) == 0))
    {
        st->fd_cng_reset_flag = 1;
        if (st->hFdCngEnc_fx != NULL && st->hFdCngEnc_fx->hFdCngCom != NULL)
        {
            /* The float code resets msFrCnt_init_counter and init_old here;
             * those fields are not present in the shim FD_CNG_COM. */
        }
    }
    else if (st->fd_cng_reset_flag > 0 && st->fd_cng_reset_flag < 10)
    {
        st->fd_cng_reset_flag = add(st->fd_cng_reset_flag, 1);
    }
    else
    {
        st->fd_cng_reset_flag = 0;
    }
}

/* -----------------------------------------------------------------------------
 * perform_noise_estimation_enc
 *
 * TODO: Full FX port of the float minimum-statistics noise estimator.
 * The current implementation is a link-unblock stub that leaves the encoder
 * noise estimate at zero; the downstream DTX/SID logic will still run but
 * the encoded noise level will not track the input.
 * ---------------------------------------------------------------------------- */

void perform_noise_estimation_enc(
    Word32 *band_energies,
    Word16 exp_band_energies,
    Word32 *enerBuffer,
    Word16 enerBuffer_exp,
    HANDLE_FD_CNG_ENC st)
{
    (void)band_energies;
    (void)exp_band_energies;
    (void)enerBuffer;
    (void)enerBuffer_exp;

    if (st == NULL)
    {
        return;
    }

    /* Keep the exponent fields honest so AdjustFirstSID does not divide
     * by uninitialised values. */
    st->msPeriodog_exp = 0;
    st->msNoiseEst_exp = 0;
}

/* -----------------------------------------------------------------------------
 * FdCng_exc
 *
 * FX port of the shared float FdCng_exc().  Copies the CNG LPC filter into
 * Aq, converts to LSP/LSF, smooths the CNG LSP vector, and produces a zero
 * excitation (the shim FD_CNG_COM does not contain exc_cng[]).  The BWE
 * excitation is interpolated from exc2.
 * ---------------------------------------------------------------------------- */

void FdCng_exc(
    HANDLE_FD_CNG_COM hs,
    Word16 *CNG_mode,
    Word16 L_frame,
    Word16 *lsp_old,
    Word16 first_CNG,
    Word16 *lsp_CNG,
    Word16 *Aq,
    Word16 *lsp_new,
    Word16 *lsf_new,
    Word16 *exc,
    Word16 *exc2,
    Word16 *bwe_exc)
{
    Word16 i;
    Word16 nSubfr;
    Word16 fac;

    if (hs == NULL || CNG_mode == NULL)
    {
        return;
    }

    *CNG_mode = -1;

    nSubfr = shr(L_frame, 6); /* L_frame / L_SUBFR (64) */

    /* Copy A_cng into all subframes. */
    for (i = 0; i < nSubfr; i++)
    {
        Copy(hs->A_cng, Aq + i * (M + 1), (Word16)(M + 1));
    }

    /* LPC -> LSP. */
    E_LPC_a_lsp_conversion(Aq, lsp_new, lsp_old, M);

    if (first_CNG == 0)
    {
        Copy(lsp_old, lsp_CNG, M);
    }

    /* AR low-pass smoothing: lspCNG = 0.9*lspCNG + 0.1*lsp_new. */
    fac = sub(32767, CNG_ISF_FACT_FX); /* 0.1 in Q15 */
    for (i = 0; i < M; i++)
    {
        Word32 acc;
        acc = L_mult(CNG_ISF_FACT_FX, lsp_CNG[i]);       /* Q30 */
        acc = L_mac(acc, fac, lsp_new[i]);               /* Q30 */
        lsp_CNG[i] = extract_l(L_shr(acc, 15));          /* Q15 */
    }

    /* LSP -> LSF. */
    E_LPC_lsp_lsf_conversion(lsp_new, lsf_new, M);

    /* Zero excitation (no exc_cng[] in the shim layout). */
    set16_fx(exc, 0, L_frame);
    set16_fx(exc2, 0, L_frame);

    /* BWE excitation interpolation. */
    if (sub(L_frame, L_FRAME) == 0)
    {
        interp_code_5over2_fx(exc2, bwe_exc, L_frame);
    }
    else
    {
        interp_code_4over2_fx(exc2, bwe_exc, L_frame);
    }
}

/* -----------------------------------------------------------------------------
 * FdCng_encodeSID
 *
 * TODO: Full FX port of the float MSVQ SID encoder.  The current stub pushes
 * zero-valued indices so that the bitstream writer has the expected number of
 * bits for an SID frame.  The spectral envelope and gain are therefore not
 * transmitted correctly; this is acceptable for link-unblocking but must be
 * replaced with a real fixed-point implementation later.
 * ---------------------------------------------------------------------------- */

void FdCng_encodeSID(HANDLE_FD_CNG_ENC st, Encoder_State_fx *corest, Word16 preemph_fac)
{
    Word16 i;

    (void)preemph_fac;

    if (st == NULL || corest == NULL)
    {
        return;
    }

    if (sub(corest->codec_mode, MODE2) == 0)
    {
        for (i = 0; i < stages_37bits; i++)
        {
            push_next_indice_fx(corest, 0, bits_37bits[i]);
        }
        push_next_indice_fx(corest, 0, 7);
    }
    else
    {
        push_indice_fx(corest, IND_SID_TYPE, 1, 1);
        push_indice_fx(corest, IND_ACELP_16KHZ, corest->bwidth_fx, 2);
        push_indice_fx(corest, IND_ACELP_16KHZ,
                       (sub(corest->L_frame_fx, L_FRAME16k) == 0) ? 1 : 0, 1);

        for (i = 0; i < stages_37bits; i++)
        {
            push_indice_fx(corest, IND_LSF, 0, bits_37bits[i]);
        }

        push_indice_fx(corest, IND_ENERGY, 0, 7);
    }
}

/* -----------------------------------------------------------------------------
 * generate_comfort_noise_enc
 *
 * TODO: Full FX port of the float STFT comfort-noise generator.  The current
 * stub clears the time-domain output buffer and leaves lp_ener_fx unchanged.
 * The shim FD_CNG_COM lacks fftBuffer/olapWinSyn/exc_cng, so the overlap-add
 * synthesis cannot be performed without extending the shim.
 * ---------------------------------------------------------------------------- */

void generate_comfort_noise_enc(Encoder_State_fx *stcod, Word16 Q_new, Word16 gen_exc)
{
    HANDLE_FD_CNG_COM st;

    (void)Q_new;
    (void)gen_exc;

    if (stcod == NULL || stcod->hFdCngEnc_fx == NULL || stcod->hFdCngEnc_fx->hFdCngCom == NULL)
    {
        return;
    }

    st = stcod->hFdCngEnc_fx->hFdCngCom;
    set16_fx(st->timeDomainBuffer, 0, L_FRAME16k);
}

/* -----------------------------------------------------------------------------
 * noisy_speech_detection
 *
 * This symbol is shared between the encoder and decoder FD-CNG paths.  The
 * implementation lives in fdcng_dec_fx.c so that both sides resolve against
 * a single definition; fdcng_enc_fx.c intentionally does not provide a
 * duplicate.  A prototype is available via prot_fx.h.
 * ---------------------------------------------------------------------------- */
