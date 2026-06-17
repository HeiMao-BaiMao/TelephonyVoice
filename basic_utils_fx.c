/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Provides five foundational fixed-point helpers that are referenced by the
 *  vendored 3GPP EVS FX tree but have no usable FX implementation in the
 *  vendored snapshot:
 *
 *      void hp20          (Word16 signal[], const Word16 stride,
 *                          const Word16 lg, Word32 mem[5],
 *                          const Word32 sFreq);
 *      void lag_wind      (Word16 r_h[], Word16 r_l[], Word16 m,
 *                          Word32 sr, Word16 strength);
 *      void adapt_lag_wind(Word16 r_h[], Word16 r_l[], Word16 m,
 *                          const Word16 Top, const Word16 Tnc,
 *                          Word32 sr);
 *      void fft16         (Word32 *re, Word32 *im, Word16 s, Word16 bScale);
 *      void BASOP_cfft    (Word32 *re, Word32 *im, Word16 sizeOfFft,
 *                          Word16 s, Word16 *scale,
 *                          Word32 workBuffer[2*BASOP_CFFT_MAX_LENGTH]);
 *
 *  Why these parent-side helpers exist
 *  -----------------------------------
 *  The upstream float implementations live under external/3gpp-evs/lib_com/
 *  (hp50.c, lag_wind.c, fft.c) and have float-based ABIs that cannot be
 *  linked into the FX static libraries.  The vendored fixed-point snapshot
 *  does not ship the corresponding FX implementations, so unresolved-symbol
 *  errors block the FX build.  Because external/3gpp-evs/ is read-only, the
 *  helpers are provided in the parent repo and attached to evs-lib-com-fx
 *  via EVS_FX_EXTRAS_LIB_COM in cmake/3gpp-evs.cmake.
 *
 *  Numerical contract
 *  ------------------
 *  The implementations below are "link-unblock" approximations.  They aim to
 *  be functionally reasonable so that the FX encoder/decoder can link and
 *  run; bit-exact fidelity to the missing upstream FX reference is a later
 *  refinement goal.
 *
 *  hp20:        2nd-order Butterworth high-pass filter at 20 Hz.  Coefficients
 *               are converted to Q14 and state values are kept in Q16 to match
 *               the documented mem[] layout (mem[2..3] store x[-2..-1]<<16).
 *               All intermediate arithmetic uses 64-bit accumulators.
 *
 *  lag_wind:    Multiplies DPF-format autocorrelations
 *               L_Comp(r_h[i], r_l[i]) by the matching DPF lag-window table
 *               and splits the Q31 product back into high/low parts.
 *
 *  adapt_lag_wind: Selects lag-window strength from open-loop pitch lag/gain
 *               (pitch gain assumed Q15) and dispatches to lag_wind.
 *
 *  fft16:       Wraps the existing upstream fixed-point complex FFT core
 *               DoRTFTn_fx() after gathering strided input into contiguous
 *               temporary arrays.  The bScale parameter is honoured as a
 *               final right-shift on the outputs (callers currently pass 0).
 *
 *  BASOP_cfft:  For power-of-two sizes supported by DoRTFTn_fx (16/32/64/
 *               128/256/512) the existing FX FFT core is used directly.
 *               For other sizes a generic complex DFT is synthesised with
 *               floating-point sin/cos as a link-unblock fallback; the
 *               result is scaled by 1/N and converted back to Word32.  This
 *               fallback is marked with a TODO because a proper fixed-point
 *               prime-factor/radix implementation would be preferable.
 *
 *  Implementation notes
 *  --------------------
 *  Only "typedefs.h" is included (matching the other parent helpers).  The
 *  FX typedef shim force-includes the Word16/Word32/basic-op environment.
 *  DoRTFTn_fx() is declared via an extern prototype because it is part of
 *  the existing FX library and already linked into evs-lib-com-fx.
 * ============================================================================ */

#include "typedefs.h"
#include <math.h>   /* for generic DFT fallback in BASOP_cfft */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ----------------------------------------------------------------------------
 *  hp20 - 2nd-order Butterworth high-pass filter at 20 Hz
 * ---------------------------------------------------------------------------- */

/* Coefficients in Q14 (computed from the float reference in hp50.c). */
#define HP20_8000_A1  ((Word16)32404)
#define HP20_8000_A2  ((Word16)-16024)
#define HP20_8000_B1  ((Word16)-32406)
#define HP20_8000_B2  ((Word16)16203)
#define HP20_16000_A1 ((Word16)32586)
#define HP20_16000_A2 ((Word16)-16203)
#define HP20_16000_B1 ((Word16)-32587)
#define HP20_16000_B2 ((Word16)16293)
#define HP20_32000_A1 ((Word16)32677)
#define HP20_32000_A2 ((Word16)-16293)
#define HP20_32000_B1 ((Word16)-32677)
#define HP20_32000_B2 ((Word16)16339)
#define HP20_48000_A1 ((Word16)32707)
#define HP20_48000_A2 ((Word16)-16323)
#define HP20_48000_B1 ((Word16)-32707)
#define HP20_48000_B2 ((Word16)16354)

void hp20(Word16 signal[], const Word16 stride, const Word16 lg,
          Word32 mem[5], const Word32 sFreq)
{
    Word16 i;
    Word16 a1, a2, b1, b2;
    Word32 y1, y2, x0, x1, x2;
    Word32 y0;
    long long acc;

    /* Select coefficients for the sampling rate. */
    if (sFreq == 8000)
    {
        a1 = HP20_8000_A1; a2 = HP20_8000_A2;
        b1 = HP20_8000_B1; b2 = HP20_8000_B2;
    }
    else if (sFreq == 16000)
    {
        a1 = HP20_16000_A1; a2 = HP20_16000_A2;
        b1 = HP20_16000_B1; b2 = HP20_16000_B2;
    }
    else if (sFreq == 32000)
    {
        a1 = HP20_32000_A1; a2 = HP20_32000_A2;
        b1 = HP20_32000_B1; b2 = HP20_32000_B2;
    }
    else
    {
        /* Default to 48 kHz coefficients for any other rate (the upstream
         * float implementation treats every non-8/16/32 kHz case as 48 kHz). */
        a1 = HP20_48000_A1; a2 = HP20_48000_A2;
        b1 = HP20_48000_B1; b2 = HP20_48000_B2;
    }

    /* Load filter state.  mem[] layout (from prot_fx.h):
     *   mem[0]: y[-2] in Q16, mem[1]: y[-1] in Q16,
     *   mem[2]: x[-2]<<16,   mem[3]: x[-1]<<16,
     *   mem[4]: states scale (we keep it at 16). */
    y2 = mem[0];
    y1 = mem[1];
    x2 = mem[2];   /* already x[-2] << 16, treated as Q16 */
    x1 = mem[3];   /* already x[-1] << 16 */

    for (i = 0; i < lg; i++)
    {
        /* New input in Q16; then shift the input delay line. */
        x0 = ((Word32)signal[i * stride]) << 16;

        /* y0 = a1*y1 + a2*y2 + b2*x0 + b1*x1 + b2*x2  (Q14 * Q16 = Q30) */
        acc  = (long long)a1 * (long long)y1;
        acc += (long long)a2 * (long long)y2;
        acc += (long long)b2 * (long long)x0;
        acc += (long long)b1 * (long long)x1;
        acc += (long long)b2 * (long long)x2;

        /* Convert Q30 accumulator to Q16. */
        y0 = (Word32)(acc >> 14);

        /* Store the filtered sample back as Q0 (round from Q16). */
        signal[i * stride] = (Word16)((y0 + 0x8000L) >> 16);

        /* Shift delay line for next iteration. */
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
    }

    /* Save state for next frame.
     * After the final iteration x1 == x[lg-1]<<16 and x2 == x[lg-2]<<16. */
    mem[0] = y2;          /* y[lg-2] in Q16 */
    mem[1] = y1;          /* y[lg-1] in Q16 */
    mem[2] = x2;          /* x[lg-2] << 16  */
    mem[3] = x1;          /* x[lg-1] << 16  */
    mem[4] = 16;          /* Q16 state scale */

    return;
}


/* ----------------------------------------------------------------------------
 *  lag_wind - apply lag-window tables to autocorrelations (DPF format)
 * ---------------------------------------------------------------------------- */

/* Lag-window strength constants from cnst_fx.h (also visible via the shim). */
#ifndef LAGW_WEAK
#define LAGW_WEAK     0
#define LAGW_MEDIUM   1
#define LAGW_STRONG   2
#endif

/* Compose a 32-bit DPF value: hi<<16 + lo<<1 (same as upstream L_Comp). */
static Word32 lag_compose(Word16 hi, Word16 lo)
{
    return (((Word32)hi) << 16) + (((Word32)lo) << 1);
}

/* Split a 32-bit DPF value into high/low 16-bit parts. */
static void lag_extract(Word32 v, Word16 *hi, Word16 *lo)
{
    *hi = (Word16)(v >> 16);
    *lo = (Word16)((v - (((Word32)(*hi)) << 16)) >> 1);
}

/* Forward declarations for the FX lag-window tables (from rom_com_fx.h). */
extern const Word16 lag_window_8k[2][16];
extern const Word16 lag_window_12k8[3][2][16];
extern const Word16 lag_window_16k[3][2][16];
extern const Word16 lag_window_25k6[3][2][16];
extern const Word16 lag_window_32k[3][2][16];
extern const Word16 lag_window_48k[2][16];

void lag_wind(Word16 r_h[], Word16 r_l[], Word16 m, Word32 sr, Word16 strength)
{
    Word16 i;
    Word32 r, w, prod;
    const Word16 (*wnd)[16];

    /* Map sampling rate and strength to the matching FX table. */
    switch (sr)
    {
    case 8000:
        wnd = lag_window_8k;
        break;
    case 12800:
        wnd = lag_window_12k8[strength];
        break;
    case 16000:
        wnd = lag_window_16k[strength];
        break;
    case 24000:
    case 25600:
        wnd = lag_window_25k6[strength];
        break;
    case 32000:
        wnd = lag_window_32k[strength];
        break;
    case 48000:
        wnd = lag_window_48k;
        break;
    default:
        /* Unknown rate: leave autocorrelations unchanged. */
        return;
    }

    for (i = 0; i <= m; i++)
    {
        r = lag_compose(r_h[i], r_l[i]);
        w = lag_compose(wnd[0][i], wnd[1][i]);
        /* Q31 * Q31 -> Q62, keep the high 31 bits. */
        prod = (Word32)(((long long)r * (long long)w) >> 31);
        lag_extract(prod, &r_h[i], &r_l[i]);
    }

    return;
}


/* ----------------------------------------------------------------------------
 *  adapt_lag_wind - choose lag-window strength from open-loop pitch lag/gain
 * ---------------------------------------------------------------------------- */

void adapt_lag_wind(Word16 r_h[], Word16 r_l[], Word16 m,
                    const Word16 Top, const Word16 Tnc, Word32 sr)
{
    Word16 strength;

    /* Pitch gain Tnc is assumed Q15; thresholds 0.6 and 0.3 map as follows. */
    if (Top < 80)
    {
        if (Tnc > (Word16)(0.6f * 32768.0f + 0.5f))  /* ~19661 in Q15 */
        {
            strength = LAGW_STRONG;
        }
        else
        {
            strength = LAGW_MEDIUM;
        }
    }
    else if (Top < 160)
    {
        if (Tnc > (Word16)(0.3f * 32768.0f + 0.5f))  /* ~9830 in Q15 */
        {
            strength = LAGW_MEDIUM;
        }
        else
        {
            strength = LAGW_WEAK;
        }
    }
    else
    {
        strength = LAGW_WEAK;
    }

    lag_wind(r_h, r_l, m, sr, strength);
    return;
}


/* ----------------------------------------------------------------------------
 *  FFT helpers
 * ---------------------------------------------------------------------------- */

/* Existing upstream FX complex-FFT core (declared in prot_fx.h). */
extern void DoRTFTn_fx(Word32 *x, Word32 *y, const Word16 n);

/* log2 for the power-of-two sizes we support. */
static Word16 log2_pow2(Word16 n)
{
    Word16 log = 0;
    while (n > 1)
    {
        n >>= 1;
        log++;
    }
    return log;
}

static Word32 saturate32(long long v)
{
    if (v > 0x7fffffffLL)
    {
        return (Word32)0x7fffffffL;
    }
    if (v < -0x80000000LL)
    {
        return (Word32)0x80000000L;
    }
    return (Word32)v;
}

/* fft16 - 16-point complex FFT with strided access.
 * The stride s is honoured by gathering/scattering; bScale is applied as a
 * final arithmetic right-shift (callers currently pass 0). */
void fft16(Word32 *re, Word32 *im, Word16 s, Word16 bScale)
{
    Word32 tmp_re[16];
    Word32 tmp_im[16];
    Word16 i;

    for (i = 0; i < 16; i++)
    {
        tmp_re[i] = re[i * s];
        tmp_im[i] = im[i * s];
    }

    DoRTFTn_fx(tmp_re, tmp_im, 16);

    if (bScale > 0)
    {
        for (i = 0; i < 16; i++)
        {
            re[i * s] = tmp_re[i] >> bScale;
            im[i * s] = tmp_im[i] >> bScale;
        }
    }
    else if (bScale < 0)
    {
        for (i = 0; i < 16; i++)
        {
            re[i * s] = saturate32((long long)tmp_re[i] << (-bScale));
            im[i * s] = saturate32((long long)tmp_im[i] << (-bScale));
        }
    }
    else
    {
        for (i = 0; i < 16; i++)
        {
            re[i * s] = tmp_re[i];
            im[i * s] = tmp_im[i];
        }
    }

    return;
}


/* BASOP_cfft - variable-size complex FFT.
 * Power-of-two sizes are dispatched to the existing DoRTFTn_fx core.
 * Non-power-of-two sizes use a generic complex DFT implemented with double
 * precision sin/cos as a link-unblock fallback (TODO: replace with a proper
 * fixed-point factorisation). */
void BASOP_cfft(Word32 *re, Word32 *im, Word16 sizeOfFft, Word16 s,
                Word16 *scale, Word32 workBuffer[2*BASOP_CFFT_MAX_LENGTH])
{
    Word16 i, k, n;
    Word16 isPow2;
    Word16 log2n;

    (void)workBuffer; /* used only by the generic-DFT fallback */

    n = sizeOfFft;

    /* Quick power-of-two test. */
    isPow2 = (n > 0) && ((n & (n - 1)) == 0);

    if (isPow2 && n >= 16 && n <= 512)
    {
        if (s == 1)
        {
            DoRTFTn_fx(re, im, n);
        }
        else
        {
            Word32 *tmp_re = workBuffer;
            Word32 *tmp_im = workBuffer + n;
            for (i = 0; i < n; i++)
            {
                tmp_re[i] = re[i * s];
                tmp_im[i] = im[i * s];
            }
            DoRTFTn_fx(tmp_re, tmp_im, n);
            for (i = 0; i < n; i++)
            {
                re[i * s] = tmp_re[i];
                im[i * s] = tmp_im[i];
            }
        }

        /* Book-keeping: reflect the FFT growth in the scale factor.
         * DoRTFTn_fx does not scale internally, so the output has grown by
         * log2(n) bits.  This mirrors the float BASOP_cfft behaviour of
         * adding SCALEFACTOR64 for its fixed 64-point transform. */
        log2n = log2_pow2(n);
        *scale = (Word16)((Word16)(*scale) + log2n);
    }
    else
    {
        /* TODO: link-unblock fallback.  Generic O(N^2) complex DFT using
         * floating-point twiddle factors.  Not bit-exact and not suitable
         * for real-time use, but it satisfies the ABI and lets the link
         * proceed so that remaining symbol gaps can be exposed. */
        double *dre = (double *)workBuffer;
        double *dim = (double *)workBuffer + BASOP_CFFT_MAX_LENGTH;

        for (i = 0; i < n; i++)
        {
            dre[i] = (double)re[i * s];
            dim[i] = (double)im[i * s];
        }

        for (k = 0; k < n; k++)
        {
            double acc_re = 0.0;
            double acc_im = 0.0;
            for (i = 0; i < n; i++)
            {
                double theta = -2.0 * M_PI * (double)i * (double)k / (double)n;
                double c = cos(theta);
                double si = sin(theta);
                acc_re += dre[i] * c - dim[i] * si;
                acc_im += dre[i] * si + dim[i] * c;
            }
            /* Scale by 1/N to keep the result in the same ballpark as the
             * input magnitude; this is an approximation of the normalisation
             * that a real BASOP_cfft would apply through *scale. */
            acc_re /= (double)n;
            acc_im /= (double)n;
            re[k * s] = saturate32((long long)acc_re);
            im[k * s] = saturate32((long long)acc_im);
        }

        /* Leave scale unchanged for the fallback. */
    }

    return;
}
