/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS FD-CNG decoder family.
 *
 *  Provides the ten fixed-point symbols that the vendored 3GPP EVS FX tree
 *  references but does not ship implementations for:
 *
 *      createFdCngDec
 *      initFdCngDec
 *      deleteFdCngDec
 *      configureFdCngDec
 *      ApplyFdCng
 *      FdCng_decodeSID
 *      generate_comfort_noise_dec
 *      generate_comfort_noise_dec_hf
 *      generate_masking_noise          (also used by encoder / post-filter)
 *      noisy_speech_detection          (also used by encoder side)
 *
 *  Why this helper exists
 *  ----------------------
 *  The upstream implementations live in external/3gpp-evs/lib_dec/fd_cng_dec.c
 *  and lib_com/fd_cng_com.c, both of which are written against the float
 *  Decoder_State / FD_CNG_COM / FD_CNG_DEC layouts.  They are not compiled
 *  into the FX static libraries, so the FX decoder cannot link.  Because
 *  external/3gpp-evs/ is read-only, the parent repo provides this file.
 *
 *  Numerical contract
 *  ------------------
 *  The implementations below are primarily "link-unblock" stubs.  They set up
 *  the FX-native FD_CNG_COM / FD_CNG_DEC structures with sensible defaults so
 *  that the decoder initializes and runs without crashing.  Comfort-noise and
 *  masking-noise generation are reduced to no-ops (zero output) for now, and
 *  the noisy-speech detector keeps its initial "clean" state.  Full fixed-point
 *  ports of the FFT-based CNG synthesis and minimum-statistics noise estimator
 *  are left as later TODOs.
 *
 *  Implementation notes
 *  --------------------
 *  The FX typedef shim force-includes the Word16/Word32 environment, the FX
 *  constants (cnst_fx.h) and the FX-native FD_CNG_COM / FD_CNG_DEC structs.
 *  We still need prot_fx.h here because this helper accesses the struct fields
 *  and calls bitstream helpers (get_next_indice_fx) that are declared there.
 * ============================================================================ */

#include "typedefs.h"
#include "prot_fx.h"
#include <stdlib.h>

/* ----------------------------------------------------------------------------
 *  createFdCngDec
 * ---------------------------------------------------------------------------- */
void createFdCngDec(HANDLE_FD_CNG_DEC* hFdCngDec)
{
    HANDLE_FD_CNG_DEC hs;

    hs = (HANDLE_FD_CNG_DEC)calloc(1, sizeof(FD_CNG_DEC));
    if (hs == NULL)
    {
        *hFdCngDec = NULL;
        return;
    }

    hs->hFdCngCom = (HANDLE_FD_CNG_COM)calloc(1, sizeof(FD_CNG_COM));
    if (hs->hFdCngCom == NULL)
    {
        free(hs);
        *hFdCngDec = NULL;
        return;
    }

    *hFdCngDec = hs;
    return;
}

/* ----------------------------------------------------------------------------
 *  initFdCngDec
 *
 *  Initialise the decoder-side FD-CNG state.  The float init uses a scale
 *  argument to compute a scaling factor; in this stub the scale is ignored and
 *  the FX-only fftlenFac slot is left at a neutral Q15 value.
 * ---------------------------------------------------------------------------- */
Word16 initFdCngDec(HANDLE_FD_CNG_DEC hFdCngDec, Word16 scale)
{
    HANDLE_FD_CNG_COM hsCom;

    (void)scale;

    if (hFdCngDec == NULL || hFdCngDec->hFdCngCom == NULL)
    {
        return -1;
    }

    hsCom = hFdCngDec->hFdCngCom;

    /* Zero every field in both structures.  set16_fx with a length expressed in
     * 16-bit units also clears the embedded 32-bit arrays. */
    set16_fx((Word16*)hFdCngDec, 0, (Word16)(sizeof(FD_CNG_DEC) / sizeof(Word16)));
    set16_fx((Word16*)hsCom, 0, (Word16)(sizeof(FD_CNG_COM) / sizeof(Word16)));

    /* The zeroing above cleared the hFdCngCom pointer; restore it. */
    hFdCngDec->hFdCngCom = hsCom;

    /* Long-term speech/noise level estimates in Q9.23, matching the float
     * initial values of -20 dB and +25 dB. */
    hFdCngDec->lp_noise  = (Word32)(-20L * (1L << 23));
    hFdCngDec->lp_speech = (Word32)( 25L * (1L << 23));

    /* Default state of the common CNG structure. */
    hsCom->frame_type_previous     = ACTIVE_FRAME;
    hsCom->flag_noisy_speech       = 0;
    hsCom->likelihood_noisy_speech = 0;
    hsCom->active_frame_counter    = 0;
    hsCom->A_cng[0]                = 4096;      /* 1.0 in Q12 */

    return 0;
}

/* ----------------------------------------------------------------------------
 *  deleteFdCngDec
 * ---------------------------------------------------------------------------- */
void deleteFdCngDec(HANDLE_FD_CNG_DEC* hFdCngDec)
{
    HANDLE_FD_CNG_DEC hsDec;

    if (hFdCngDec == NULL)
    {
        return;
    }

    hsDec = *hFdCngDec;
    if (hsDec != NULL)
    {
        if (hsDec->hFdCngCom != NULL)
        {
            free(hsDec->hFdCngCom);
            hsDec->hFdCngCom = NULL;
        }
        free(hsDec);
        *hFdCngDec = NULL;
    }

    return;
}

/* ----------------------------------------------------------------------------
 *  configureFdCngDec
 *
 *  Mirrors the bandwidth/brate dependent setup of the float configure path,
 *  but only sets the fields that the FX decoder actually reads.  The full
 *  partition / window tables are omitted because the FX-native FD_CNG_COM
 *  struct does not carry them and no FX call site currently needs them.
 * ---------------------------------------------------------------------------- */
void configureFdCngDec(HANDLE_FD_CNG_DEC hsDec,
                       Word16 bandwidth,
                       Word32 bitrate,
                       Word16 L_frame)
{
    HANDLE_FD_CNG_COM hsCom;

    if (hsDec == NULL || hsDec->hFdCngCom == NULL)
    {
        return;
    }

    hsCom = hsDec->hFdCngCom;

    hsCom->CngBandwidth = bandwidth;
    if (hsCom->CngBandwidth == FB)
    {
        hsCom->CngBandwidth = SWB;
    }

    if (L_sub(bitrate, FRAME_NO_DATA) != 0 && L_sub(bitrate, SID_2k40) != 0)
    {
        hsCom->CngBitrate = bitrate;
    }

    hsCom->numSlots = 16;

    /* Bandwidth-dependent setup selection. */
    if (bandwidth == NB)
    {
        hsCom->FdCngSetup    = FdCngSetup_nb;
        hsCom->numCoreBands  = 16;
        hsCom->regularStopBand = 16;
    }
    else if (bandwidth == WB)
    {
        if (L_sub(hsCom->CngBitrate, ACELP_8k00) <= 0 && L_frame == L_FRAME)
        {
            hsCom->FdCngSetup      = FdCngSetup_wb1;
            hsCom->numCoreBands    = 16;
            hsCom->regularStopBand = 16;
        }
        else if (L_sub(hsCom->CngBitrate, ACELP_13k20) <= 0 || L_frame == L_FRAME)
        {
            hsCom->FdCngSetup = FdCngSetup_wb2;
            hsCom->numCoreBands    = 16;
            hsCom->regularStopBand = 20;
            if (L_frame == L_FRAME16k)
            {
                hsCom->numCoreBands    = 20;
                hsCom->regularStopBand = 20;
                /* The FX setup table already carries fftlen=512/stopFFTbin=256
                 * for wb2; override to the 16k-frame variant. */
                hsCom->FdCngSetup.fftlen    = 640;
                hsCom->FdCngSetup.stopFFTbin = 256;
            }
        }
        else
        {
            hsCom->FdCngSetup      = FdCngSetup_wb3;
            hsCom->numCoreBands    = 20;
            hsCom->regularStopBand = 20;
        }
    }
    else
    {
        /* SWB / FB */
        if (L_frame == L_FRAME)
        {
            hsCom->FdCngSetup      = FdCngSetup_swb1;
            hsCom->numCoreBands    = 16;
            hsCom->regularStopBand = 35;
        }
        else
        {
            hsCom->FdCngSetup      = FdCngSetup_swb2;
            hsCom->numCoreBands    = 20;
            hsCom->regularStopBand = 40;
        }
    }

    hsCom->fftlen    = hsCom->FdCngSetup.fftlen;
    hsCom->stopFFTbin = hsCom->FdCngSetup.stopFFTbin;

    hsCom->startBand = 2;
    if (hsCom->FdCngSetup.numPartitions > 0)
    {
        hsCom->stopBand = hsCom->FdCngSetup.sidPartitions[hsCom->FdCngSetup.numPartitions - 1] + 1;
        hsCom->npart    = hsCom->FdCngSetup.numPartitions;
    }
    else
    {
        hsCom->stopBand = hsCom->stopFFTbin;
        hsCom->npart    = 0;
    }

    hsCom->frameSize = shr(hsCom->fftlen, 1);

    /* Neutral Q15 factor.  The real FX value would derive from the overlap-add
     * synthesis scaling; since comfort-noise generation is currently a stub,
     * this value is not exercised. */
    hsCom->fftlenFac = 16384;   /* 0.5 in Q15 */

    return;
}

/* ----------------------------------------------------------------------------
 *  ApplyFdCng
 *
 *  The float version performs minimum-statistics noise estimation and updates
 *  the CNG level buffers.  This stub only keeps the high-level state machine
 *  consistent (frame-type history, DTX flag) so that downstream logic does not
 *  operate on stale state.
 * ---------------------------------------------------------------------------- */
Word16 ApplyFdCng(Word16* timeDomainInput,
                  Word16 Q,
                  Word32** cldfbBufferReal,
                  Word32** cldfbBufferImag,
                  Word16* cldfbBufferScale,
                  HANDLE_FD_CNG_DEC st,
                  Word16 m_frame_type,
                  Decoder_State_fx* stdec,
                  const Word16 concealWholeFrame,
                  Word16 is_music)
{
    HANDLE_FD_CNG_COM hsCom;

    (void)timeDomainInput;
    (void)Q;
    (void)cldfbBufferReal;
    (void)cldfbBufferImag;
    (void)cldfbBufferScale;
    (void)stdec;
    (void)concealWholeFrame;
    (void)is_music;

    if (st == NULL || st->hFdCngCom == NULL)
    {
        return 0;
    }

    hsCom = st->hFdCngCom;

    if (hsCom->frame_type_previous == ACTIVE_FRAME)
    {
        hsCom->active_frame_counter = 0;
    }

    if (m_frame_type == ACTIVE_FRAME)
    {
        hsCom->active_frame_counter = 0;
        hsCom->frame_type_previous  = ACTIVE_FRAME;
    }
    else if (m_frame_type == SID_FRAME)
    {
        st->flag_dtx_mode = 1;
        hsCom->frame_type_previous = SID_FRAME;
    }
    else if (m_frame_type == ZERO_FRAME)
    {
        hsCom->frame_type_previous = ZERO_FRAME;
    }

    return 0;
}

/* ----------------------------------------------------------------------------
 *  FdCng_decodeSID
 *
 *  Decode an FD-CNG SID frame.  The float version performs an MSVQ decode of
 *  the spectral envelope and a gain decode.  This stub consumes the same number
 *  of bits from the decoder bitstream so that the read pointer stays aligned,
 *  but does not write the decoded noise level.
 * ---------------------------------------------------------------------------- */
void FdCng_decodeSID(HANDLE_FD_CNG_COM st, Decoder_State_fx* corest)
{
    Word16 i;

    (void)st;

    if (corest == NULL)
    {
        return;
    }

    /* Consume the MSVQ indices. */
    for (i = 0; i < stages_37bits; i++)
    {
        (void)get_next_indice_fx(corest, bits_37bits[i]);
    }

    /* Consume the 7-bit gain index. */
    (void)get_next_indice_fx(corest, 7);

    return;
}

/* ----------------------------------------------------------------------------
 *  generate_comfort_noise_dec
 *
 *  Generate FFT-domain and CLDFB-domain comfort noise.  This stub leaves the
 *  time-domain buffer zero and, if requested, clears the CLDFB noise bands.
 * ---------------------------------------------------------------------------- */
void generate_comfort_noise_dec(Word32** bufferReal,
                                Word32** bufferImag,
                                Word16* bufferScale,
                                Decoder_State_fx* stdec,
                                Word16* Q_new,
                                Word16 gen_exc)
{
    HANDLE_FD_CNG_COM hsCom;
    Word16 i, j;

    (void)bufferScale;
    (void)Q_new;
    (void)gen_exc;

    if (stdec == NULL || stdec->hFdCngDec_fx == NULL || stdec->hFdCngDec_fx->hFdCngCom == NULL)
    {
        return;
    }

    hsCom = stdec->hFdCngDec_fx->hFdCngCom;

    /* Keep the overlap-add synthesis buffers at zero so that no stale signal
     * is mixed into the output. */
    if (hsCom->frameSize > 0)
    {
        set16_fx(hsCom->timeDomainBuffer, 0, hsCom->frameSize);
    }

    /* Clear the high-band CLDFB contribution when the caller supplies buffers. */
    if (bufferReal != NULL && bufferImag != NULL &&
        hsCom->numCoreBands < hsCom->regularStopBand)
    {
        for (i = 0; i < hsCom->numSlots; i++)
        {
            for (j = hsCom->numCoreBands; j < hsCom->regularStopBand; j++)
            {
                bufferReal[i][j] = 0;
                bufferImag[i][j] = 0;
            }
        }
    }

    return;
}

/* ----------------------------------------------------------------------------
 *  generate_comfort_noise_dec_hf
 *
 *  Generate comfort noise for the CLDFB high band only.  Stubbed as zero fill.
 * ---------------------------------------------------------------------------- */
void generate_comfort_noise_dec_hf(Word32** bufferReal,
                                   Word32** bufferImag,
                                   Word16* bufferScale,
                                   Decoder_State_fx* stdec)
{
    HANDLE_FD_CNG_COM hsCom;
    Word16 i, j;

    (void)bufferScale;

    if (stdec == NULL || stdec->hFdCngDec_fx == NULL || stdec->hFdCngDec_fx->hFdCngCom == NULL)
    {
        return;
    }

    hsCom = stdec->hFdCngDec_fx->hFdCngCom;

    if (bufferReal == NULL || bufferImag == NULL)
    {
        return;
    }

    if (hsCom->numCoreBands < hsCom->regularStopBand)
    {
        for (j = hsCom->numCoreBands; j < hsCom->regularStopBand; j++)
        {
            for (i = 0; i < hsCom->numSlots; i++)
            {
                bufferReal[i][j] = 0;
                bufferImag[i][j] = 0;
            }
        }
    }

    return;
}

/* ----------------------------------------------------------------------------
 *  generate_masking_noise
 *
 *  Shared helper that adds extra comfort noise on top of decoded active speech.
 *  This stub updates the random seed but leaves the time-domain buffer
 *  unchanged, effectively disabling the extra noise fill until a proper fixed-
 *  point synthesis is available.
 * ---------------------------------------------------------------------------- */
void generate_masking_noise(Word16* timeDomainBuffer,
                            Word16 Q,
                            HANDLE_FD_CNG_COM st,
                            Word16 length,
                            Word16 core)
{
    Word16 i;

    (void)timeDomainBuffer;
    (void)Q;
    (void)length;
    (void)core;

    if (st == NULL)
    {
        return;
    }

    /* Cheap LCG seed update so the seed is not stuck at its initial value. */
    for (i = 0; i < 8; i++)
    {
        st->seed = (Word16)((st->seed * 31821) + 13849);
    }

    return;
}

/* ----------------------------------------------------------------------------
 *  noisy_speech_detection
 *
 *  Shared helper used by both encoder and decoder VAD/CNA paths.  This stub
 *  keeps the long-term level estimates at their initial values and always
 *  reports "not noisy" so that the comfort-noise addition path stays disabled.
 * ---------------------------------------------------------------------------- */
void noisy_speech_detection(const Word16 vad,
                            const Word16* ftimeInPtr,
                            const Word16 frameSize,
                            const Word16 Q,
                            const Word32* msNoiseEst,
                            const Word16 msNoiseEst_exp,
                            const Word16* psize_norm,
                            const Word16 psize_norm_exp,
                            const Word16 nFFTpart,
                            Word32* lp_noise,
                            Word32* lp_speech,
                            Word16* flag_noisy_speech)
{
    (void)vad;
    (void)ftimeInPtr;
    (void)frameSize;
    (void)Q;
    (void)msNoiseEst;
    (void)msNoiseEst_exp;
    (void)psize_norm;
    (void)psize_norm_exp;
    (void)nFFTpart;
    (void)lp_noise;
    (void)lp_speech;

    if (flag_noisy_speech != NULL)
    {
        *flag_noisy_speech = 0;
    }

    return;
}
