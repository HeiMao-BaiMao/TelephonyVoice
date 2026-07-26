/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Implements the seven TEC/TFA TBE FX symbols declared in
 *      external/3gpp-evs/lib_com/prot_fx.h (lines ~10770-10836)
 *  that are referenced by the vendored FX encoder/decoder but have no
 *  upstream fixed-point implementation in the vendored snapshot:
 *
 *      void  tfaCalcEnv_fx(...)
 *      Word16 tfaEnc_TBE_fx(...)
 *      void  tecEnc_TBE_fx(...)
 *      void  set_TEC_TFA_code_fx(...)
 *      Word16 procTecTfa_TBE_Fx(...)
 *      void  calcGainTemp_TBE_Fx(...)
 *      void  calcLoEnvCheckCorrHiLo_Fix(...)
 *
 *  Why this parent-side helper exists
 *  ----------------------------------
 *  The upstream float implementations live in
 *      external/3gpp-evs/lib_com/tec_com.c
 *      external/3gpp-evs/lib_enc/tfa_enc.c
 *  and pull in the float prot.h / typedef.h / cnst.h chain.  Because
 *  external/3gpp-evs/ is read-only in this repo, the FX port lives here
 *  and is attached to evs-lib-com-fx via EVS_FX_EXTRAS_LIB_COM.
 *
 *  Porting strategy
 *  ----------------
 *  The float reference works in the dB domain (10*log10(power)) for
 *  envelope estimation and in the linear domain for TEC/TFA sample
 *  scaling.  This FX port uses the existing BASOP logarithm helpers
 *  (BASOP_Util_Log2, BASOP_util_Pow2) and keeps intermediate envelope
 *  values in a signed Q6 dB representation (1 dB == 64).  The Q6 scale
 *  was chosen so that the dynamic range of typical CLDFB/SHB energies
 *  (roughly -150 dB .. +150 dB) fits comfortably in a Word16, and so
 *  that the dB-domain thresholds from the float reference convert to
 *  small integer values.
 *
 *  Key constants (float -> FX Q6 dB)
 *      800  dB^2  -> 800  * 64^2 = 3276800
 *      720  dB^2  -> 720  * 64^2 = 2949120
 *      100  dB    -> 100  * 64   = 6400
 *      20   dB    -> 20   * 64   = 1280
 *
 *  dB <-> log2 conversion
 *      dB        = log2(x) * 10*log10(2)   (10*log10(2) ~= 3.0103)
 *      dB_Q6     = log2_Q25(x) * 12330 >> 31
 *      log2_Q25  = dB_Q6 * 174166 >> 15    (approximate)
 *
 *  where 12330 = round(10*log10(2) * 64 * 2^31 / 2^25) and
 *  174166 = round(2^25 / (10*log10(2) * 64)).
 *
 *  Known limitations / conservative approximations
 *  -------------------------------------------------
 *    - The encoder-side hiTempEnv buffer is assumed to have been filled
 *      by upstream CLDFB analysis in the same Q6 dB format as loBuffer.
 *      If the upstream FX CLDFB path does not populate it, correlation
 *      decisions will be based on stale/uninitialized data.
 *    - TEC/TFA sample scaling (procTecTfa_TBE_Fx) uses a simplified
 *      energy-domain gain computation compared with the float reference.
 *      It preserves the overall algorithm shape (energy normalization
 *      for TFA, gain application with bounding for TEC) but is not
 *      bit-exact and may saturate aggressively in extreme cases.
 *    - The correlation-coefficient branch in calcLoEnvCheckCorrHiLo_Fix
 *      is implemented with plain fixed-point math; the threshold
 *      comparisons are exact to the chosen Q6 representation, but the
 *      intermediate covariance/variances use 64-bit accumulators that
 *      may differ slightly from a fully BASOP-normalized reference.
 * ============================================================================ */

#include "options.h"
#include "cnst_fx.h"
#include "prot_fx.h"
#include "rom_com_fx.h"
#include "rom_basop_util.h"
#include "stl.h"

#ifndef ABS
#define ABS(x) ((x) >= 0 ? (x) : -(x))
#endif

/* --------------------------------------------------------------------------
 *  Local constants
 * -------------------------------------------------------------------------- */

/* Number of low-band groups used by TEC/TFA. */
#define NB_TEC_LOW_BAND  3

/* Low-band smoothing coefficients.  TecSC_Fx is stored as coef*2 in Q15,
 * so a multiply/accumulate followed by a >> 16 gives the true
 * coefficient contribution. */
#define TEC_SC_SHIFT     16

/* Q6 dB <-> log2(Q25) conversion constants. */
#define LOG2_TO_DB_Q6    12330   /* round(10*log10(2) * 64 * 2^6)  -> dB_Q6 = log2_Q25 * C >> 31 */
#define DB_Q6_TO_LOG2    174166  /* round(2^25 / (10*log10(2)*64)) -> log2_Q25 = dB_Q6 * C >> 15 */

/* Ratio scaling factors for the float thresholds. */
#define RATIO_HI_LO_FAC_DEC_Q15  39061  /* 1.5894f * 0.75f in Q15 */
#define FAC_NS_TBE_Q15           45875  /* 1.4f in Q15 */

/* dB-domain thresholds converted to the Q6 representation. */
#define TH_HI_VAR_Q12     3276800L   /*  800 dB^2 */
#define TH_LO_VAR_NS_Q12  2949120L   /*  720 dB^2 */
#define TH_DIFF_Q6        6400       /*  100 dB   */
#define TH_FEATURE_Q6     1280       /*   20 dB   */

/* Flatness thresholds in log2(Q25). */
#define LOG2_0_70_Q25     (-17262800L)   /* log2(0.70) * 2^25 */
#define LOG2_0_50_Q25     (-33554432L)   /* log2(0.50) * 2^25 */

/* Voicing / pitch thresholds in their native Q formats. */
#define VOICING_0_70_Q15  22938   /* 0.70  * 32768 */
#define VOICING_1_10_Q15  36045   /* 1.10  * 32768 */
#define VOICING_0_20_Q15   6554   /* 0.20  * 32768 */
#define VOICING_1_20_Q15  39322   /* 1.20  * 32768 (clipped representation; compare in Word32) */
#define VOICING_1_40_Q15  45875   /* 1.40  * 32768 (sum of two Q15 values; compare in Word32) */
#define PITCH_440_Q6      28160   /* 440.0 * 64    */

/* Energy lower limit for TFA (10000 / N_TEC_TFA_SUBFR actual value). */
#define TFA_MA_BOTTOM     625

/* --------------------------------------------------------------------------
 *  Saturating 32-bit helpers (plain C, 64-bit intermediate)
 * -------------------------------------------------------------------------- */

static Word16 saturate16(long long v)
{
    if (v > 32767LL)  return 32767;
    if (v < -32768LL) return -32768;
    return (Word16)v;
}

static Word32 saturate32(long long v)
{
    if (v > 0x7FFFFFFFLL)  return 0x7FFFFFFFL;
    if (v < -0x80000000LL) return 0x80000000L;
    return (Word32)v;
}

/* Integer square root of a non-negative 32-bit value. Returns floor(sqrt(v)). */
static Word32 isqrt32(Word32 v)
{
    Word32 r = 0;
    Word32 bit = 1UL << 30;

    if (v == 0) return 0;
    while (bit > v) bit >>= 2;
    while (bit != 0)
    {
        if (v >= r + bit)
        {
            v -= r + bit;
            r = (r >> 1) + bit;
        }
        else
        {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* --------------------------------------------------------------------------
 *  Convert log2(Q25) to Q6 dB.
 * -------------------------------------------------------------------------- */
static Word16 log2_to_db_q6(Word32 log2_q25)
{
    long long v = (long long)log2_q25 * LOG2_TO_DB_Q6;
    v >>= 31;
    return saturate16(v);
}

/* --------------------------------------------------------------------------
 *  Convert a CLDFB subband power (Q(2*cldfb_exp) integer) to Q6 dB.
 * -------------------------------------------------------------------------- */
static Word16 power_to_db_q6(Word32 power_q, Word16 cldfb_exp)
{
    Word32 log2_q25;
    Word16 q_offset;

    if (power_q <= 0) power_q = 1;

    log2_q25 = BASOP_Util_Log2(power_q);          /* log2(power_q) in Q25 */
    q_offset = (Word16)(cldfb_exp << 1);          /* 2*cldfb_exp */
    /* Convert the Q offset to Q25 using a 64-bit multiply to avoid
     * signed-left-shift UB for negative cldfb_exp values. */
    log2_q25 = L_sub(log2_q25,
                     saturate32((long long)q_offset * 33554432LL));

    return log2_to_db_q6(log2_q25);
}

/* --------------------------------------------------------------------------
 *  Convert Q6 dB to linear gain as (mantissa Q15, exponent) using BASOP_util_Pow2.
 *  Output convention: value = gain_m * 2^(gain_e - 15).
 * -------------------------------------------------------------------------- */
static void db_q6_to_linear_gain(Word16 db_q6, Word16 *gain_m, Word16 *gain_e)
{
    Word32 log2_q25;
    Word16 norm;
    Word32 mant_q31;

    if (db_q6 == 0)
    {
        /* 0 dB -> gain = 1.0: 0.5 Q15 with exponent 1. */
        *gain_m = (Word16)0x4000;   /* 0.5 in Q15 */
        *gain_e = 1;
        return;
    }

    /* log2(gain) = db_q6 / (10*log10(2) * 64)  -> Q25 */
    log2_q25 = (Word32)((long long)db_q6 * DB_Q6_TO_LOG2 >> 15);

    /* Normalize to Q31 for BASOP_util_Pow2. */
    norm = norm_l(log2_q25);
    if (norm == 0) norm = 1;  /* keep at least one headroom bit */
    log2_q25 = L_shl(log2_q25, norm);

    mant_q31 = BASOP_util_Pow2(log2_q25, sub(25, norm), gain_e);

    /* Convert Q31 mantissa to Q15 and keep exponent unchanged. */
    *gain_m = (Word16)(mant_q31 >> 16);
}

/* --------------------------------------------------------------------------
 *  Set up subframe configuration (i_offset -> k_offset, n_subfr).
 * -------------------------------------------------------------------------- */
static void set_subfr_config(Word16 i_offset, Word16 *k_offset, Word16 *n_subfr, Word16 l_subfr)
{
    *n_subfr = (Word16)(N_TEC_TFA_SUBFR - i_offset);
    *k_offset = (Word16)(i_offset * l_subfr);
}

/* --------------------------------------------------------------------------
 *  Compute subframe energies of a Q15 time-domain buffer.
 *  Energies are returned in Q(2*input_exp).
 * -------------------------------------------------------------------------- */
static Word32 calc_subfr_nrg(
    Word16 *hb_synth,
    Word16  i_offset,
    Word32 *enr,
    Word16  k_offset,
    Word16  l_subfr
)
{
    Word16 i, j, k;
    long long enr_all = 1;  /* small epsilon, same as float 1e-12f */

    for (i = i_offset, k = k_offset; i < N_TEC_TFA_SUBFR; i++)
    {
        long long nrg = 0;
        for (j = 0; j < l_subfr; j++, k++)
        {
            long long s = (long long)hb_synth[k];
            nrg += s * s;
        }
        if (nrg == 0) nrg = 1;
        if (nrg > 0x7FFFFFFFLL) nrg = 0x7FFFFFFFLL;
        enr[i] = (Word32)nrg;
        enr_all += nrg;
    }
    if (enr_all > 0x7FFFFFFFLL) enr_all = 0x7FFFFFFFLL;
    return (Word32)enr_all;
}

/* ======================================================================== *
 *  1. tfaCalcEnv_fx
 * ======================================================================== */
void tfaCalcEnv_fx(const Word16* shb_speech, Word32* enr)
{
    Word16 i, j, k;

    for (i = 0, k = 0; i < N_TEC_TFA_SUBFR; i++)
    {
        long long nrg = 0;
        for (j = 0; j < L_TEC_TFA_SUBFR16k; j++, k++)
        {
            long long s = (long long)shb_speech[k];
            nrg += s * s;
        }
        if (nrg == 0) nrg = 1;                     /* epsilon */
        if (nrg > 0x7FFFFFFFLL) nrg = 0x7FFFFFFFLL;
        enr[i] = (Word32)nrg;                       /* Q(2*Q_shb_spch) */
    }
}

/* ======================================================================== *
 *  2. tfaEnc_TBE_fx
 * ======================================================================== */
Word16 tfaEnc_TBE_fx(
    Word32* enr,
    Word16  last_core,
    Word16* voicing,
    Word16* pitch_buf,
    Word16  Q_enr
)
{
    Word16 i;
    Word32 sum_enr = 0;
    Word32 sum_log2_enr = 0;       /* Q25 */
    Word32 flatness_log2_q25;
    Word32 m_a;
    Word16 tfa_flag = 0;
    Word32 voice_sum, pitch_buf_sum;

    for (i = 0; i < N_TEC_TFA_SUBFR; i++)
    {
        sum_enr = L_add(sum_enr, enr[i]);
        sum_log2_enr = L_add(sum_log2_enr, BASOP_Util_Log2(enr[i]));
    }

    m_a = L_shr(sum_enr, 4);       /* arithmetic mean, still Q_enr */

    /* Flatness = geometric_mean / arithmetic_mean, computed in log2 domain.
     * log2(flatness) = avg(log2(enr)) - log2(sum_enr) + log2(16). */
    flatness_log2_q25 = L_sub(L_shr(sum_log2_enr, 4), BASOP_Util_Log2(sum_enr));
    flatness_log2_q25 = L_add(flatness_log2_q25, 134217728L);  /* +4 in Q25 */

    voice_sum     = (Word32)voicing[0] + (Word32)voicing[1];
    pitch_buf_sum = (Word32)pitch_buf[0] + (Word32)pitch_buf[1]
                  + (Word32)pitch_buf[2] + (Word32)pitch_buf[3];

    if (((flatness_log2_q25 > LOG2_0_70_Q25) &&
         (pitch_buf_sum > PITCH_440_Q6) &&
         (voice_sum > VOICING_1_40_Q15)) ||          /* 1.40 = 2.0*0.70 */
        ((last_core == TCX_20_CORE) &&
         (flatness_log2_q25 > LOG2_0_50_Q25) &&
         (voice_sum < VOICING_1_40_Q15)))
    {
        tfa_flag = 1;
    }

    /* Energy lower limit: m_a_actual < 625.  Compare in 64-bit with Q_enr scaling. */
    {
        long long ma = (long long)m_a;
        long long thr = 625LL << Q_enr;
        if (ma < thr)
        {
            tfa_flag = 0;
        }
    }

    return tfa_flag;
}

/* ======================================================================== *
 *  3. tecEnc_TBE_fx
 * ======================================================================== */
void tecEnc_TBE_fx(Word16* corrFlag, const Word16* voicing, Word16 coder_type)
{
    Word32 voice_sum;
    Word32 voice_diff;

    voice_sum  = (Word32)voicing[0] + (Word32)voicing[1];
    voice_diff = (Word32)voicing[0] - (Word32)voicing[1];
    if (voice_diff < 0) voice_diff = -voice_diff;

    if (*corrFlag == 1)
    {
        if ((coder_type == INACTIVE) ||
            ((voice_sum > VOICING_0_70_Q15) &&
             (voice_sum < VOICING_1_10_Q15) &&
             (voice_diff < VOICING_0_20_Q15)))
        {
            *corrFlag = 0;
        }
    }

    if (voice_sum > VOICING_1_20_Q15)
    {
        *corrFlag = 0;
    }
}

/* ======================================================================== *
 *  4. set_TEC_TFA_code_fx
 * ======================================================================== */
void set_TEC_TFA_code_fx(const Word16 corrFlag, Word16* tec_flag, Word16* tfa_flag)
{
    *tec_flag = 0;
    if (*tfa_flag == 0)
    {
        if (corrFlag == 1)
        {
            *tec_flag = 1;
        }
        else if (corrFlag == 2)
        {
            *tec_flag = 1;
            *tfa_flag = 1;
        }
    }
}

/* ======================================================================== *
 *  5. procTecTfa_TBE_Fx
 * ======================================================================== */
Word16 procTecTfa_TBE_Fx(
    Word16 *hb_synth_Fx,
    Word16  hb_synth_fx_exp,
    Word16 *gain_m,
    Word16 *gain_e,
    Word16  flat_flag,
    Word16  last_core,
    Word16  l_subfr,
    Word16  code
)
{
    Word16 i, j, k;
    Word16 i_offset = 0;
    Word16 k_offset, n_subfr;
    Word32 enr[N_TEC_TFA_SUBFR];
    Word32 enr_ave;
    (void)code;  /* reserved for stricter gain limiting; not used in this conservative port */

    if (flat_flag != 0)
    {
        /* TFA: flatten subframe energies to their average. */
        set_subfr_config(0, &k_offset, &n_subfr, l_subfr);
        enr_ave = L_shr(calc_subfr_nrg(hb_synth_Fx, 0, enr, k_offset, l_subfr), 4);

        for (i = 0, k = 0; i < N_TEC_TFA_SUBFR; i++)
        {
            long long ratio64;
            Word32 ratio;
            Word16 gain;

            if (enr[i] == 0) enr[i] = 1;
            /* ratio = enr_ave / enr[i]  in Q30; sqrt gives Q15 gain. */
            ratio64 = ((long long)enr_ave << 30) / enr[i];
            if (ratio64 > 0x7FFFFFFFLL) ratio64 = 0x7FFFFFFFLL;
            if (ratio64 < 0) ratio64 = 0;
            ratio = (Word32)ratio64;
            gain = (Word16)isqrt32(ratio);  /* Q15 */

            for (j = 0; j < l_subfr; j++, k++)
            {
                long long v = (long long)hb_synth_Fx[k] * gain;
                v >>= 15;  /* Q15 gain back to sample Q */
                hb_synth_Fx[k] = saturate16(v);
            }
        }
    }
    else
    {
        /* TEC: apply decoded temporal envelope gains. */
        if (last_core != ACELP_CORE)
        {
            i_offset = 1;
        }

        set_subfr_config(i_offset, &k_offset, &n_subfr, l_subfr);
        enr_ave = L_shr(calc_subfr_nrg(hb_synth_Fx, i_offset, enr, k_offset, l_subfr),
                        (Word16)(N_TEC_TFA_SUBFR - i_offset));

        for (i = i_offset, k = k_offset; i < N_TEC_TFA_SUBFR; i++)
        {
            long long ratio64;
            Word32 ratio;
            Word16 corr_gain;     /* Q15 energy-correction sqrt factor */
            Word32 prod;
            Word16 gain_q15;
            Word16 out_gain_e;
            Word16 shift;

            if (enr[i] == 0) enr[i] = 1;

            /* Energy correction factor: sqrt(enr_ave / enr[i]) in Q15. */
            ratio64 = ((long long)enr_ave << 30) / enr[i];
            if (ratio64 > 0x7FFFFFFFLL) ratio64 = 0x7FFFFFFFLL;
            if (ratio64 < 0) ratio64 = 0;
            ratio = (Word32)ratio64;
            corr_gain = (Word16)isqrt32(ratio);

            /* Combine decoded gain mantissa (Q15) with correction factor (Q15). */
            prod = (long long)gain_m[i] * corr_gain;
            prod >>= 15;
            gain_q15 = (Word16)saturate16(prod);
            out_gain_e = gain_e[i];

            /* Apply gain = gain_q15 * 2^(out_gain_e - 15) to samples. */
            for (j = 0; j < l_subfr; j++, k++)
            {
                long long v = (long long)hb_synth_Fx[k] * gain_q15;
                v >>= 15;  /* remove Q15 mantissa */

                if (out_gain_e >= 0)
                {
                    v <<= out_gain_e;
                }
                else
                {
                    shift = (Word16)(-out_gain_e);
                    v >>= shift;
                }
                hb_synth_Fx[k] = saturate16(v);
            }
        }
    }

    return hb_synth_fx_exp;
}

/* ======================================================================== *
 *  6. calcGainTemp_TBE_Fx
 * ======================================================================== */
void calcGainTemp_TBE_Fx(
    Word32** pCldfbRealSrc_Fx,
    Word32** pCldfbImagSrc_Fx,
    Word16   cldfb_exp,
    Word16*  loBuffer_Fx,
    Word16   startPos,
    Word16   stopPos,
    Word16   lowSubband,
    Word16*  pGainTemp_m,
    Word16*  pGainTemp_e,
    Word16   code
)
{
    const Word16 bw_lo = TecLowBandTable[NB_TEC_LOW_BAND];  /* = 6 */
    Word16 slot, lb, k, li, ui;
    Word16 band_offset;
    Word16 no_cols;
    Word16 lo_temp_env[CLDFB_NO_COL_MAX];

    band_offset = sub(lowSubband, bw_lo);
    no_cols = sub(stopPos, startPos);

    /* Compute low-band energy per slot and store as Q6 dB. */
    for (slot = startPos; slot < stopPos; slot++)
    {
        long long nrg = 0;

        for (lb = 0; lb < NB_TEC_LOW_BAND; lb++)
        {
            li = TecLowBandTable[lb];
            ui = TecLowBandTable[lb + 1];

            for (k = li; k < ui; k++)
            {
                long long re = (long long)pCldfbRealSrc_Fx[slot][k + band_offset];
                long long im = (long long)pCldfbImagSrc_Fx[slot][k + band_offset];
                nrg += re * re + im * im;
            }
            /* Average over the width of this group (ui-li == 2 for the
             * fixed TecLowBandTable).  Implemented as a 64-bit multiply
             * by BASOP_util_normReciprocal[2] (0.5 in Q31) >> 31. */
            nrg = (nrg * (long long)BASOP_util_normReciprocal[ui - li]) >> 31;
        }

        if (nrg == 0) nrg = 1;
        if (nrg > 0x7FFFFFFFLL) nrg = 0x7FFFFFFFLL;

        loBuffer_Fx[MAX_TEC_SMOOTHING_DEG + slot] = power_to_db_q6((Word32)nrg, cldfb_exp);
    }

    /* Calculate smoothed temporal envelope and linear gains if requested. */
    if (code > 0)
    {
        Word16 adj_fac_q15;
        Word16 base;

        if (code != 2)
        {
            adj_fac_q15 = RATIO_HI_LO_FAC_DEC_Q15;
        }
        else
        {
            adj_fac_q15 = FAC_NS_TBE_Q15;
        }

        /* Smoothing with one-slot delay (TBE variant). */
        for (slot = 0; slot < no_cols; slot++)
        {
            long long acc = 0;
            for (k = 0; k <= MAX_TEC_SMOOTHING_DEG; k++)
            {
                acc += (long long)TecSC_Fx[k]
                     * (long long)loBuffer_Fx[MAX_TEC_SMOOTHING_DEG + slot - 1 - k];
            }
            acc >>= TEC_SC_SHIFT;                 /* undo coef*2 Q15 storage */
            acc = (acc * adj_fac_q15) >> 15;      /* apply adjustment factor */
            lo_temp_env[slot] = saturate16(acc);
        }

        /* Convert Q6 dB envelope to linear gain per slot. */
        base = add(MAX_TEC_SMOOTHING_DEG, startPos);
        for (slot = startPos; slot < stopPos; slot++)
        {
            db_q6_to_linear_gain(lo_temp_env[slot - startPos],
                                 &pGainTemp_m[slot],
                                 &pGainTemp_e[slot]);
        }
        (void)base;  /* kept for readability; slot indexing is absolute */
    }

    /* Update history: shift the last MAX_TEC_SMOOTHING_DEG slots to the front. */
    for (k = 0; k < MAX_TEC_SMOOTHING_DEG; k++)
    {
        loBuffer_Fx[k] = loBuffer_Fx[stopPos + k];
    }
}

/* ======================================================================== *
 *  7. calcLoEnvCheckCorrHiLo_Fix
 * ======================================================================== */
void calcLoEnvCheckCorrHiLo_Fix(
    Word16 noCols,
    Word16* pFreqBandTable,
    Word16* loBuffer_Fix,
    Word16* loTempEnv_Fix,
    Word16* loTempEnv_ns_Fix,
    Word16* hiTempEnvOrig_Fix,
    Word16* corrFlag
)
{
    Word16 i, j;
    Word16* hiTempEnv = hiTempEnvOrig_Fix + EXT_DELAY_HI_TEMP_ENV;
    Word32 hi_sum = 0;
    Word32 hi_var = 0;
    Word32 lo_sum_ns = 0;
    Word32 lo_var_ns = 0;
    Word32 lo_sum = 0;
    Word32 lo_var = 0;
    Word32 cov = 0;
    Word16 code = 0;
    Word32 diff_hi_lo_sum;
    (void)pFreqBandTable;  /* only used for assertions in the float reference */

    /* Compute hiTempEnv variance and sum (Q6 input -> Q12 variance). */
    for (i = 0; i < noCols; i++)
    {
        hi_sum = L_add(hi_sum, (Word32)hiTempEnv[i]);
    }
    for (i = 0; i < noCols; i++)
    {
        Word32 d = L_sub((Word32)hiTempEnv[i], L_shr(hi_sum, 4));  /* centered, Q6 */
        Word32 prod = d * d;                                       /* Q12 */
        hi_var = L_add(hi_var, L_shr(prod, 4));                    /* accumulate Q12 */
    }

    /* Compute loTempEnv (smoothed + scaled) and loTempEnv_ns (1.4x, delayed). */
    for (i = 0; i < noCols; i++)
    {
        long long acc = 0;
        for (j = 0; j <= MAX_TEC_SMOOTHING_DEG; j++)
        {
            acc += (long long)TecSC_Fx[j]
                 * (long long)loBuffer_Fix[MAX_TEC_SMOOTHING_DEG + i - 1 - j];
        }
        acc >>= TEC_SC_SHIFT;
        acc = (acc * RATIO_HI_LO_FAC_DEC_Q15) >> 15;
        loTempEnv_Fix[i] = saturate16(acc);

        /* Non-smoothed variant with 1.4x factor and one-slot delay. */
        loTempEnv_ns_Fix[i] = saturate16(((long long)loBuffer_Fix[MAX_TEC_SMOOTHING_DEG + i - 1]
                                        * FAC_NS_TBE_Q15) >> 15);
    }

    /* Compute loTempEnv_ns variance and sum. */
    for (i = 0; i < noCols; i++)
    {
        lo_sum_ns = L_add(lo_sum_ns, (Word32)loTempEnv_ns_Fix[i]);
    }
    for (i = 0; i < noCols; i++)
    {
        Word32 d = L_sub((Word32)loTempEnv_ns_Fix[i], L_shr(lo_sum_ns, 4));
        Word32 prod = d * d;
        lo_var_ns = L_add(lo_var_ns, L_shr(prod, 4));
    }

    diff_hi_lo_sum = L_sub(lo_sum_ns, hi_sum);

    /* Classify tentative code. */
    if ((hi_var > TH_HI_VAR_Q12) && (lo_var_ns > TH_LO_VAR_NS_Q12)
        && (diff_hi_lo_sum < TH_DIFF_Q6))
    {
        code = 1;
    }

    *corrFlag = 0;

    if (code != 0)
    {
        /* code == 1: peak-position check and local dynamic-range feature. */
        Word16 max_pos_hi = 0, max_pos_lo = 0;
        Word16 max_hi = hiTempEnv[0];
        Word16 max_lo = loTempEnv_ns_Fix[0];
        Word16 feature_max = 0;
        Word16 pos_feature_max = 0;
        Word16 feature[16];
        Word16 len_window = EXT_DELAY_HI_TEMP_ENV + 1;  /* = 3 */
        Word16* curr_pos = hiTempEnv;

        for (i = 1; i < noCols; i++)
        {
            if (hiTempEnv[i] > max_hi)
            {
                max_hi = hiTempEnv[i];
                max_pos_hi = i;
            }
            if (loTempEnv_ns_Fix[i] > max_lo)
            {
                max_lo = loTempEnv_ns_Fix[i];
                max_pos_lo = i;
            }
        }

        if (ABS(max_pos_hi - max_pos_lo) < 2)
        {
            *corrFlag = 2;
        }

        for (i = 0; i < 16; i++, curr_pos++)
        {
            Word16 min_local = curr_pos[0];
            Word16 max_local = curr_pos[0];
            for (j = 1; j < len_window; j++)
            {
                if (max_local < curr_pos[-j]) max_local = curr_pos[-j];
                if (min_local > curr_pos[-j]) min_local = curr_pos[-j];
            }
            feature[i] = saturate16((long long)max_local - (long long)min_local);
            if (feature[i] > feature_max)
            {
                feature_max = feature[i];
                pos_feature_max = i;
            }
        }

        if (*corrFlag > 0)
        {
            if (!((feature_max > TH_FEATURE_Q6) && (ABS(pos_feature_max - max_pos_hi) < 3)))
            {
                *corrFlag = 0;
            }
        }
    }
    else
    {
        /* code == 0: correlation-coefficient + variance-ratio test. */
        Word32 corrCoef_q15;
        Word16 norm;

        /* Compute loTempEnv variance and sum. */
        for (i = 0; i < noCols; i++)
        {
            lo_sum = L_add(lo_sum, (Word32)loTempEnv_Fix[i]);
        }
        for (i = 0; i < noCols; i++)
        {
            Word32 d = L_sub((Word32)loTempEnv_Fix[i], L_shr(lo_sum, 4));
            Word32 prod = d * d;
            lo_var = L_add(lo_var, L_shr(prod, 4));
        }

        /* Covariance between hiTempEnv and loTempEnv. */
        for (i = 0; i < noCols; i++)
        {
            Word32 dh = L_sub((Word32)hiTempEnv[i], L_shr(hi_sum, 4));
            Word32 dl = L_sub((Word32)loTempEnv_Fix[i], L_shr(lo_sum, 4));
            Word32 prod = dh * dl;
            cov = L_add(cov, L_shr(prod, 4));
        }

        /* corrCoef = cov / sqrt(hi_var * lo_var)  in Q15. */
        {
            long long prod = (long long)hi_var * (long long)lo_var;
            Word32 sqrt_prod = isqrt32((Word32)(prod >> 12));  /* sqrt(Q24) -> Q12 approx */
            if (sqrt_prod == 0) sqrt_prod = 1;
            corrCoef_q15 = (Word32)(((long long)cov << 15) / sqrt_prod);
        }

        /* Variance ratio test using 64-bit integer scaling to avoid Q-format
         * overflow for the upper threshold (2.0288). */
        if ((corrCoef_q15 >= 28823) &&              /* 0.8795 in Q15 */
            (((long long)hi_var * 10000LL) > ((long long)lo_var * 3649LL)) &&
            (((long long)hi_var * 10000LL) < ((long long)lo_var * 20288LL)))
        {
            *corrFlag = 1;
        }
    }

    /* Update history buffers. */
    for (i = 0; i < MAX_TEC_SMOOTHING_DEG + DELAY_TEMP_ENV_BUFF_TEC; i++)
    {
        loBuffer_Fix[i] = loBuffer_Fix[noCols + i];
    }
    for (i = 0; i < DELAY_TEMP_ENV_BUFF_TEC + EXT_DELAY_HI_TEMP_ENV; i++)
    {
        hiTempEnvOrig_Fix[i] = hiTempEnvOrig_Fix[noCols + i];
    }
}
