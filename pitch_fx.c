/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Implements the eight pitch-related FX symbols declared in
 *      external/3gpp-evs/lib_com/prot_fx.h
 *  that are referenced by the vendored FX encoder/decoder but have no
 *  upstream fixed-point implementation in the vendored snapshot:
 *
 *      void  pitch_ol_init_fx(...)
 *      void  pitch_ol_fx(...)
 *      Word16 pit_decode_fx(...)
 *      void  pit_Q_dec_fx(...)
 *      void  pit16k_Q_dec_fx(...)
 *      void  abs_pit_dec_fx(...)
 *      void  delta_pit_dec_fx(...)
 *      void  pitch_pred_linear_fit(...)
 *
 *  Why this parent-side helper exists
 *  ----------------------------------
 *  The upstream float implementations live in
 *      external/3gpp-evs/lib_enc/pitch_ol.c
 *      external/3gpp-evs/lib_dec/pit_dec.c
 *      external/3gpp-evs/lib_dec/pitch_extr.c
 *  and pull in the float prot.h / typedef.h / cnst.h chain.  Because
 *  external/3gpp-evs/ is read-only in this repo, the FX port lives here
 *  and is attached to evs-lib-com-fx via EVS_FX_EXTRAS_LIB_COM.
 *
 *  Porting strategy
 *  ----------------
 *  - pitch_ol_init_fx: trivial fixed-point initialization.
 *  - pitch_ol_fx:      complex open-loop pitch analysis; provided as a
 *                      minimal link-unblock stub that returns a safe
 *                      default pitch and zero voicing.
 *  - pit_decode_fx,
 *    pit_Q_dec_fx,
 *    pit16k_Q_dec_fx,
 *    abs_pit_dec_fx,
 *    delta_pit_dec_fx: direct integer-arithmetic ports of the float
 *                      pitch decoders in lib_dec/pit_dec.c.  They use
 *                      the FX bitstream helpers (get_next_indice_fx,
 *                      BIT_ALLOC_IDX_fx) and the FX limit_T0_fx helper.
 *  - pitch_pred_linear_fit: link-unblock stub that returns the last known
 *                      pitch lag clipped to the allowed range; the full
 *                      weighted linear-fit extrapolation from
 *                      lib_dec/pitch_extr.c is not implemented.
 *
 *  Known limitations / TODOs
 *  -------------------------
 *  - pitch_ol_fx is a stub and does not perform meaningful open-loop
 *    pitch analysis; it merely returns linkable, non-crashing defaults.
 *  - pitch_pred_linear_fit is a stub and simply reuses the last known pitch
 *    lag; it does not perform the weighted linear-fit extrapolation.
 * ============================================================================ */

#include "typedefs.h"
#include "options.h"
#include "cnst_fx.h"
#include "prot_fx.h"
#include "rom_com_fx.h"

#ifndef ABS_FX
#define ABS_FX(x) ((x) >= 0 ? (x) : -(x))
#endif

/* ----------------------------------------------------------------------------
 *  pitch_ol_init_fx - open-loop pitch tracker state initialization
 * ---------------------------------------------------------------------------- */
void pitch_ol_init_fx(
    Word16 *old_thres,
    Word16 *old_pitch,
    Word16 *delta_pit,
    Word16 *old_corr
)
{
    *old_thres = 0;
    move16();
    *old_pitch = 0;
    move16();
    *delta_pit = 0;
    move16();
    *old_corr = 0;
    move16();

    return;
}

/* ----------------------------------------------------------------------------
 *  pitch_ol_fx - open-loop pitch analysis
 *
 *  TODO: This is a link-unblock stub.  It returns safe defaults and does not
 *        perform the actual open-loop pitch search from lib_enc/pitch_ol.c.
 * ---------------------------------------------------------------------------- */
void pitch_ol_fx(
    Word16 pitch[3],
    Word16 voicing[3],
    Word16 *old_pitch,
    Word16 *old_corr,
    Word16 corr_shift,
    Word16 *old_thres,
    Word16 *delta_pit,
    Word16 *st_old_wsp2,
    const Word16 *wsp,
    Word16 mem_decim2[3],
    const Word16 relE,
    const Word16 last_class,
    const Word16 bwidth,
    const Word16 Opt_SC_VBR
)
{
    Word16 i;
    (void)corr_shift;
    (void)st_old_wsp2;
    (void)wsp;
    (void)mem_decim2;
    (void)relE;
    (void)last_class;
    (void)bwidth;
    (void)Opt_SC_VBR;

    for (i = 0; i < 3; i++)
    {
        pitch[i] = L_SUBFR;
        move16();
        voicing[i] = 0;
        move16();
    }

    *old_pitch = L_SUBFR;
    move16();
    *old_corr = 0;
    move16();
    *old_thres = 0;
    move16();
    *delta_pit = 0;
    move16();

    return;
}

/* ----------------------------------------------------------------------------
 *  abs_pit_dec_fx - decode absolute pitch (integer + fractional part)
 * ---------------------------------------------------------------------------- */
void abs_pit_dec_fx(
    const Word16 fr_steps,
    Word16 pitch_index,
    const Word16 limit_flag,
    Word16 *T0,
    Word16 *T0_frac
)
{
    if (limit_flag == 0)
    {
        if (fr_steps == 2)
        {
            if (pitch_index < (PIT_FR1_8b - PIT_MIN) * 2)
            {
                *T0 = PIT_MIN + (pitch_index / 2);
                *T0_frac = pitch_index - ((*T0 - PIT_MIN) * 2);
                *T0_frac = shl(*T0_frac, 1);
            }
            else
            {
                *T0 = pitch_index + PIT_FR1_8b - ((PIT_FR1_8b - PIT_MIN) * 2);
                *T0_frac = 0;
            }
        }
        else if (fr_steps == 4)
        {
            if (pitch_index < (PIT_FR2_9b - PIT_MIN) * 4)
            {
                *T0 = PIT_MIN + (pitch_index / 4);
                *T0_frac = pitch_index - (*T0 - PIT_MIN) * 4;
            }
            else if (pitch_index < ((PIT_FR2_9b - PIT_MIN) * 4 + (PIT_FR1_9b - PIT_FR2_9b) * 2))
            {
                pitch_index = sub(pitch_index, (PIT_FR2_9b - PIT_MIN) * 4);
                *T0 = PIT_FR2_9b + (pitch_index / 2);
                *T0_frac = pitch_index - (*T0 - PIT_FR2_9b) * 2;
                *T0_frac = shl(*T0_frac, 1);
            }
            else
            {
                *T0 = pitch_index + PIT_FR1_9b - ((PIT_FR2_9b - PIT_MIN) * 4) - ((PIT_FR1_9b - PIT_FR2_9b) * 2);
                *T0_frac = 0;
            }
        }
        else  /* fr_steps == 0 */
        {
            /* not used in the codec */
        }
    }
    else if (limit_flag == 1)
    {
        if (fr_steps == 2)
        {
            if (pitch_index < (PIT_FR1_EXTEND_8b - PIT_MIN_EXTEND) * 2)
            {
                *T0 = PIT_MIN_EXTEND + (pitch_index / 2);
                *T0_frac = pitch_index - ((*T0 - PIT_MIN_EXTEND) * 2);
                *T0_frac = shl(*T0_frac, 1);
            }
            else
            {
                *T0 = pitch_index + PIT_FR1_EXTEND_8b - ((PIT_FR1_EXTEND_8b - PIT_MIN_EXTEND) * 2);
                *T0_frac = 0;
            }
        }
        else if (fr_steps == 4)
        {
            if (pitch_index < (PIT_FR2_EXTEND_9b - PIT_MIN_EXTEND) * 4)
            {
                *T0 = PIT_MIN_EXTEND + (pitch_index / 4);
                *T0_frac = pitch_index - (*T0 - PIT_MIN_EXTEND) * 4;
            }
            else if (pitch_index < ((PIT_FR2_EXTEND_9b - PIT_MIN_EXTEND) * 4 + (PIT_FR1_EXTEND_9b - PIT_FR2_EXTEND_9b) * 2))
            {
                pitch_index = sub(pitch_index, (PIT_FR2_EXTEND_9b - PIT_MIN_EXTEND) * 4);
                *T0 = PIT_FR2_EXTEND_9b + (pitch_index / 2);
                *T0_frac = pitch_index - (*T0 - PIT_FR2_EXTEND_9b) * 2;
                *T0_frac = shl(*T0_frac, 1);
            }
            else
            {
                *T0 = pitch_index + PIT_FR1_EXTEND_9b - ((PIT_FR2_EXTEND_9b - PIT_MIN_EXTEND) * 4) - ((PIT_FR1_EXTEND_9b - PIT_FR2_EXTEND_9b) * 2);
                *T0_frac = 0;
            }
        }
        else
        {
            /* fr_steps == 0, not used */
        }
    }
    else  /* limit_flag == 2 */
    {
        if (fr_steps == 2)
        {
            if (pitch_index < (PIT_FR1_DOUBLEEXTEND_8b - PIT_MIN_DOUBLEEXTEND) * 2)
            {
                *T0 = PIT_MIN_DOUBLEEXTEND + (pitch_index / 2);
                *T0_frac = pitch_index - ((*T0 - PIT_MIN_DOUBLEEXTEND) * 2);
                *T0_frac = shl(*T0_frac, 1);
            }
            else
            {
                *T0 = pitch_index + PIT_FR1_DOUBLEEXTEND_8b - ((PIT_FR1_DOUBLEEXTEND_8b - PIT_MIN_DOUBLEEXTEND) * 2);
                *T0_frac = 0;
            }
        }
        else if (fr_steps == 4)
        {
            if (pitch_index < (PIT_FR2_DOUBLEEXTEND_9b - PIT_MIN_DOUBLEEXTEND) * 4)
            {
                *T0 = PIT_MIN_DOUBLEEXTEND + (pitch_index / 4);
                *T0_frac = pitch_index - (*T0 - PIT_MIN_DOUBLEEXTEND) * 4;
            }
            else if (pitch_index < ((PIT_FR2_DOUBLEEXTEND_9b - PIT_MIN_DOUBLEEXTEND) * 4 + (PIT_FR1_DOUBLEEXTEND_9b - PIT_FR2_DOUBLEEXTEND_9b) * 2))
            {
                pitch_index = sub(pitch_index, (PIT_FR2_DOUBLEEXTEND_9b - PIT_MIN_DOUBLEEXTEND) * 4);
                *T0 = PIT_FR2_DOUBLEEXTEND_9b + (pitch_index / 2);
                *T0_frac = pitch_index - (*T0 - PIT_FR2_DOUBLEEXTEND_9b) * 2;
                *T0_frac = shl(*T0_frac, 1);
            }
            else
            {
                *T0 = pitch_index + PIT_FR1_DOUBLEEXTEND_9b - ((PIT_FR2_DOUBLEEXTEND_9b - PIT_MIN_DOUBLEEXTEND) * 4) - ((PIT_FR1_DOUBLEEXTEND_9b - PIT_FR2_DOUBLEEXTEND_9b) * 2);
                *T0_frac = 0;
            }
        }
        else
        {
            /* fr_steps == 0, not used */
        }
    }

    return;
}

/* ----------------------------------------------------------------------------
 *  delta_pit_dec_fx - decode delta pitch
 * ---------------------------------------------------------------------------- */
void delta_pit_dec_fx(
    const Word16 fr_steps,
    const Word16 pitch_index,
    Word16 *T0,
    Word16 *T0_frac,
    const Word16 T0_min
)
{
    if (fr_steps == 0)
    {
        *T0 = add(T0_min, pitch_index);
        *T0_frac = 0;
    }
    else if (fr_steps == 2)
    {
        *T0 = add(T0_min, shr(pitch_index, 1));
        *T0_frac = sub(pitch_index, shl(sub(*T0, T0_min), 1));
        *T0_frac = shl(*T0_frac, 1);
    }
    else if (fr_steps == 4)
    {
        *T0 = add(T0_min, shr(pitch_index, 2));
        *T0_frac = sub(pitch_index, shl(sub(*T0, T0_min), 2));
    }

    return;
}

/* ----------------------------------------------------------------------------
 *  pit_Q_dec_fx - pitch decoding for 12.8 kHz core
 * ---------------------------------------------------------------------------- */
void pit_Q_dec_fx(
    const Word16 Opt_AMR_WB,
    const Word16 pitch_index,
    const Word16 nBits,
    const Word16 delta,
    const Word16 pit_flag,
    const Word16 limit_flag,
    Word16 *T0,
    Word16 *T0_frac,
    Word16 *T0_min,
    Word16 *T0_max,
    Word16 *BER_detect
)
{
    if (nBits == 10)
    {
        if (limit_flag == 0)
        {
            *T0 = PIT_MIN + shr(pitch_index, 2);
            *T0_frac = sub(pitch_index, shl(sub(*T0, PIT_MIN), 2));
        }
        else if (limit_flag == 1)
        {
            *T0 = PIT_MIN_EXTEND + shr(pitch_index, 2);
            *T0_frac = sub(pitch_index, shl(sub(*T0, PIT_MIN_EXTEND), 2));
        }
        else  /* limit_flag == 2 */
        {
            *T0 = PIT_MIN_DOUBLEEXTEND + shr(pitch_index, 2);
            *T0_frac = sub(pitch_index, shl(sub(*T0, PIT_MIN_DOUBLEEXTEND), 2));
        }
    }
    else if (nBits == 9)
    {
        abs_pit_dec_fx(4, pitch_index, limit_flag, T0, T0_frac);

        if (Opt_AMR_WB)
        {
            limit_T0_fx(L_FRAME, delta, pit_flag, 0, *T0, 0, T0_min, T0_max);
        }
    }
    else if (nBits == 8)
    {
        abs_pit_dec_fx(2, pitch_index, limit_flag, T0, T0_frac);

        if (Opt_AMR_WB)
        {
            limit_T0_fx(L_FRAME, delta, pit_flag, 0, *T0, 0, T0_min, T0_max);
        }
    }
    else if (nBits == 6)
    {
        delta_pit_dec_fx(4, pitch_index, T0, T0_frac, *T0_min);
    }
    else if (nBits == 5)
    {
        if (delta == 8)
        {
            delta_pit_dec_fx(2, pitch_index, T0, T0_frac, *T0_min);
        }
        else
        {
            delta_pit_dec_fx(4, pitch_index, T0, T0_frac, *T0_min);
        }
    }
    else  /* nBits == 4 */
    {
        if (delta == 8)
        {
            delta_pit_dec_fx(0, pitch_index, T0, T0_frac, *T0_min);
        }
        else
        {
            delta_pit_dec_fx(2, pitch_index, T0, T0_frac, *T0_min);
        }
    }

    /* bit-error detection mechanism */
    if (add(shl(*T0, 2), *T0_frac) > add(shl(PIT_MAX, 2), 2) && pit_flag == 0 && !Opt_AMR_WB)
    {
        *T0 = L_SUBFR;
        *T0_frac = 0;
        *BER_detect = 1;
        move16();
    }

    if (!Opt_AMR_WB)
    {
        limit_T0_fx(L_FRAME, delta, L_SUBFR, limit_flag, *T0, *T0_frac, T0_min, T0_max);
    }

    return;
}

/* ----------------------------------------------------------------------------
 *  pit16k_Q_dec_fx - pitch decoding for 16 kHz core
 * ---------------------------------------------------------------------------- */
void pit16k_Q_dec_fx(
    const Word16 pitch_index,
    const Word16 nBits,
    const Word16 limit_flag,
    Word16 *T0,
    Word16 *T0_frac,
    Word16 *T0_min,
    Word16 *T0_max,
    Word16 *BER_detect
)
{
    Word16 index;

    (void)limit_flag;

    if (nBits == 10)
    {
        if (pitch_index < (PIT16k_FR2_EXTEND_10b - PIT16k_MIN_EXTEND) * 4)
        {
            *T0 = PIT16k_MIN_EXTEND + shr(pitch_index, 2);
            *T0_frac = sub(pitch_index, shl(sub(*T0, PIT16k_MIN_EXTEND), 2));
        }
        else
        {
            index = sub(pitch_index, (PIT16k_FR2_EXTEND_10b - PIT16k_MIN_EXTEND) * 4);
            *T0 = PIT16k_FR2_EXTEND_10b + shr(index, 1);
            *T0_frac = sub(index, shl(sub(*T0, PIT16k_FR2_EXTEND_10b), 1));
            *T0_frac = shl(*T0_frac, 1);
        }
    }
    else if (nBits == 9)
    {
        if (pitch_index < (PIT16k_FR2_EXTEND_9b - PIT16k_MIN_EXTEND) * 4)
        {
            *T0 = PIT16k_MIN_EXTEND + shr(pitch_index, 2);
            *T0_frac = sub(pitch_index, shl(sub(*T0, PIT16k_MIN_EXTEND), 2));
        }
        else if (pitch_index < ((PIT16k_FR2_EXTEND_9b - PIT16k_MIN_EXTEND) * 4 + (PIT16k_FR1_EXTEND_9b - PIT16k_FR2_EXTEND_9b) * 2))
        {
            index = sub(pitch_index, (PIT16k_FR2_EXTEND_9b - PIT16k_MIN_EXTEND) * 4);
            *T0 = PIT16k_FR2_EXTEND_9b + shr(index, 1);
            *T0_frac = sub(index, shl(sub(*T0, PIT16k_FR2_EXTEND_9b), 1));
            *T0_frac = shl(*T0_frac, 1);
        }
        else
        {
            *T0 = pitch_index + PIT16k_FR1_EXTEND_9b - ((PIT16k_FR2_EXTEND_9b - PIT16k_MIN_EXTEND) * 4) - ((PIT16k_FR1_EXTEND_9b - PIT16k_FR2_EXTEND_9b) * 2);
            *T0_frac = 0;
        }
    }
    else  /* nBits == 6 */
    {
        delta_pit_dec_fx(4, pitch_index, T0, T0_frac, *T0_min);
    }

    /* bit-error detection mechanism */
    if (add(shl(*T0, 2), *T0_frac) > shl(PIT16k_MAX, 2) && nBits >= 9)
    {
        *T0 = L_SUBFR;
        *T0_frac = 0;
        *BER_detect = 1;
        move16();
    }

    limit_T0_fx(L_FRAME16k, 8, L_SUBFR, limit_flag, *T0, *T0_frac, T0_min, T0_max);

    return;
}

/* ----------------------------------------------------------------------------
 *  pit_decode_fx - top-level OL/CL pitch lag decoding
 * ---------------------------------------------------------------------------- */
Word16 pit_decode_fx(
    Decoder_State_fx *st_fx,
    const Word32 core_brate,
    const Word16 Opt_AMR_WB,
    const Word16 L_frame,
    Word16 i_subfr,
    const Word16 coder_type,
    Word16 *limit_flag,
    Word16 *T0,
    Word16 *T0_frac,
    Word16 *T0_min,
    Word16 *T0_max,
    const Word16 L_subfr
)
{
    Word16 pitch;
    Word16 pitch_index, nBits, pit_flag;

    pitch_index = 0;
    move16();

    pit_flag = i_subfr;
    if (i_subfr == shl(L_SUBFR, 1))
    {
        pit_flag = 0;
    }

    if (!Opt_AMR_WB)
    {
        if (i_subfr == 0)
        {
            *limit_flag = 1;
            move16();

            if (coder_type == VOICED)
            {
                *limit_flag = 2;
                move16();
            }

            if (coder_type == GENERIC && L_sub(core_brate, ACELP_7k20) == 0)
            {
                *limit_flag = 0;
                move16();
            }
        }
        else if (i_subfr == shl(L_SUBFR, 1) && coder_type == GENERIC && L_sub(core_brate, ACELP_13k20) <= 0)
        {
            if (*T0 > shr(add(PIT_FR1_EXTEND_8b, PIT_MIN), 1))
            {
                *limit_flag = 0;
                move16();
            }
        }

        nBits = 0;
        move16();
        if (coder_type != AUDIO)
        {
            if (L_frame == L_FRAME)
            {
                nBits = ACB_bits_tbl[BIT_ALLOC_IDX_fx(core_brate, coder_type, i_subfr, 0)];
            }
            else
            {
                nBits = ACB_bits_16kHz_tbl[BIT_ALLOC_IDX_16KHZ_fx(core_brate, coder_type, i_subfr, 0)];
            }

            pitch_index = (Word16)get_next_indice_fx(st_fx, nBits);
        }

        if (coder_type == AUDIO)
        {
            if (L_subfr == shr(L_FRAME, 1) && i_subfr != 0)
            {
                pit_flag = L_SUBFR;
            }

            if (pit_flag == 0)
            {
                nBits = 10;
            }
            else
            {
                nBits = 6;
            }

            pitch_index = (Word16)get_next_indice_fx(st_fx, nBits);

            if (L_subfr == shr(L_FRAME, 1) && i_subfr != 0 && pitch_index >= 32)
            {
                pitch_index = shr(pitch_index, 1);
                st_fx->BER_detect = 1;
                move16();
            }

            pit_Q_dec_fx(0, pitch_index, nBits, 4, pit_flag, *limit_flag, T0, T0_frac, T0_min, T0_max, &st_fx->BER_detect);
        }
        else if (coder_type == VOICED)
        {
            if (i_subfr == shl(L_SUBFR, 1))
            {
                pit_flag = i_subfr;
            }

            pit_Q_dec_fx(0, pitch_index, nBits, 4, pit_flag, *limit_flag, T0, T0_frac, T0_min, T0_max, &st_fx->BER_detect);
        }
        else
        {
            if (L_frame == L_FRAME)
            {
                pit_Q_dec_fx(0, pitch_index, nBits, 8, pit_flag, *limit_flag, T0, T0_frac, T0_min, T0_max, &st_fx->BER_detect);
            }
            else
            {
                pit16k_Q_dec_fx(pitch_index, nBits, *limit_flag, T0, T0_frac, T0_min, T0_max, &st_fx->BER_detect);
            }
        }
    }
    else
    {
        *limit_flag = 0;
        move16();

        if (i_subfr == 0 || (i_subfr == shl(L_SUBFR, 1) && L_sub(core_brate, ACELP_8k85) == 0))
        {
            nBits = 8;
        }
        else
        {
            nBits = 5;
        }

        if (L_sub(core_brate, ACELP_8k85) > 0)
        {
            nBits = 6;

            if (i_subfr == 0 || i_subfr == shl(L_SUBFR, 1))
            {
                nBits = 9;
            }
        }

        pitch_index = (Word16)get_next_indice_fx(st_fx, nBits);

        pit_Q_dec_fx(1, pitch_index, nBits, 8, pit_flag, *limit_flag, T0, T0_frac, T0_min, T0_max, &st_fx->BER_detect);
    }

    /* floating pitch output in Q6 */
    pitch = add(shl(*T0, 6), shl(*T0_frac, 4));

    return pitch;
}

/* ----------------------------------------------------------------------------
 *  pitch_pred_linear_fit - pitch prediction for frame erasure
 *
 *  TODO: link-unblock stub.  Returns the last known pitch lag clipped to the
 *        allowed [pit_min, pit_max] range and signals that extrapolation was
 *        not performed.  The full weighted linear-fit implementation from
 *        lib_dec/pitch_extr.c is left for future work.
 * ---------------------------------------------------------------------------- */
void pitch_pred_linear_fit(
    const Word16 bfi_cnt,
    const Word16 last_good,
    Word32 *old_pitch_buf,
    Word32 *old_fpitch,
    Word32 *T0_out,
    Word16 pit_min,
    Word16 pit_max,
    Word16 *mem_pitch_gain,
    Word16 limitation,
    Word8 plc_use_future_lag,
    Word16 *extrapolationFailed,
    Word16 nb_subfr
)
{
    /* TODO: This is a link-unblock stub.  The full weighted linear-fit
     *       extrapolation from lib_dec/pitch_extr.c involves intricate
     *       fixed-point Q-format bookkeeping; for now we fall back to the
     *       reference's safe default (reuse last known pitch) and signal
     *       that extrapolation was not performed. */
    (void)bfi_cnt;
    (void)last_good;
    (void)old_pitch_buf;
    (void)mem_pitch_gain;
    (void)limitation;
    (void)plc_use_future_lag;
    (void)nb_subfr;

    if (L_sub(*old_fpitch, L_deposit_l(pit_max)) > 0)
    {
        *T0_out = L_deposit_l(pit_max);
    }
    else if (L_sub(*old_fpitch, L_deposit_l(pit_min)) < 0)
    {
        *T0_out = L_deposit_l(pit_min);
    }
    else
    {
        *T0_out = *old_fpitch;
    }

    *extrapolationFailed = 1;
    move16();

    return;
}
