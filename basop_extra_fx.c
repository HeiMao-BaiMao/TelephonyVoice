/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Provides six fixed-point BASOP utility helpers that are *called* by the
 *  vendored 3GPP EVS FX tree under external/3gpp-evs/ but have no
 *  implementation (and no prototype) in that vendored snapshot:
 *
 *      Word16 getScaleFactor16           (const Word16 *x, const Word16 len);
 *      Word16 getSqrtWord32              (Word32 val);
 *      Word16 getNormReciprocalWord16    (Word16 x);
 *      Word16 BASOP_Util_Divide3232_Scale(Word32 x, Word32 y, Word16 *s);
 *      Word32 Dot_product12_offs         (const Word16 x[], const Word16 y[],
 *                                         const Word16 lg, Word16 *exp,
 *                                         Word32 L_off);
 *      Word32 Dot_productSq16HQ          (const Word32 L_off, const Word16 x[],
 *                                         const Word16 lg, Word16 *exp);
 *
 *  Upstream equivalents exist in the IVAS/3GPP BASOP utility reference
 *  (basop_util.c / ivas_basop_util.c) but are not present in the vendored
 *  EVS reference snapshot.  The read-only submodule cannot be edited, so we
 *  provide small parent-side C implementations and attach them to
 *  evs-lib-com-fx via EVS_FX_EXTRAS_LIB_COM in cmake/3gpp-evs.cmake.
 *
 *  Key call sites (all read-only, vendored 3GPP source):
 *    getScaleFactor16
 *      lib_com/lpc_tools_fx.c:835
 *          scale = sub(getScaleFactor16(lpc, 19), SCALEFACTOR16);
 *      lib_dec/evs_dec_fx.c:961
 *          timeIn_e = s_max(0, sub(getScaleFactor16(output_sp,
 *                                                 st_fx->L_frame_fx), 3));
 *      lib_dec/evs_dec_fx.c:964
 *          timeIn_e = s_max(0, s_min(sub(getScaleFactor16(pcmbufFB,
 *                                                 st_fx->L_frameTCX), 3),
 *                                    timeIn_e));
 *    getSqrtWord32
 *      lib_com/index_pvq_opt_fx.c:672
 *          acc_val = lshr(add(1, getSqrtWord32(UL_subNsD(UL_lshl(ind_in,1),
 *                                                        1U))), 1);
 *      lib_dec/pvq_core_dec_fx.c:398,406,435,466
 *          alpha = add(getSqrtWord32(acc), density1);
 *          alpha = getSqrtWord32(dec_freq);
 *          tmp2  = getSqrtWord32(acc);
     *          tmp2  = getSqrtWord32(tmp1); (floor)
 *    getNormReciprocalWord16
 *      lib_com/window_ola_fx.c:612,664
 *          divisor = getNormReciprocalWord16(out_filt_length);
 *
 *  Batch B helpers (added below)
 *    BASOP_Util_Divide3232_Scale
 *      lib_enc/amr_wb_enc_fx.c:352
 *          tmp = BASOP_Util_Divide3232_Scale(lp_bckr, hp_bckr, &e_tmp);
 *      lib_enc/find_tilt_fx.c:86
 *          tmp = BASOP_Util_Divide3232_Scale(lp_bckr, hp_bckr, &e_tmp);
 *      lib_com/lpc_tools_fx.c:794
 *          s[1] = BASOP_Util_Divide3232_Scale(L_sub(L_tmp1,L_tmp),L_tmp3,&step);
 *      lib_enc/vad_basop.c:50
 *          result = L_deposit_h(BASOP_Util_Divide3232_Scale(L_var1, L_var2, Q_OUT));
 *    Dot_product12_offs
 *      lib_com/gain_inov.c:23
 *          L_tmp = Dot_product12_offs(code, code, lcode, &exp_L_tmp, 2621l);
 *      lib_enc/lp_exc_e_fx.c:400
 *          xx = round_fx(Dot_product12_offs(xn, xn, L_subfr, &exp_xx, 1));
 *    Dot_productSq16HQ
 *      lib_com/frame_ener_fx.c:169
 *          Ltmp = Dot_productSq16HQ(0, pt1, len, &exp1);
 *
 *  Numerical contract
 *  ------------------
 *    getScaleFactor16(x, len)
 *      Returns the headroom (number of left shifts) needed to normalize
 *      the most-significant 16-bit magnitude in the array, range [0..15],
 *      with 0 reserved for the all-zeros array.  This is the same contract
 *      as the upstream helper: scan for the maximum positive value and the
 *      minimum negative value, compute norm_s() for each non-zero extreme,
 *      and return s_min(i_max, i_min) masked to 4 bits.  The plain-C
 *      normalization helper mirrors the ITU-T G.191 norm_s() behavior:
 *      a non-zero value is shifted left until its magnitude lies in the
 *      interval [0x4000, 0x7FFF].
 *
 *    getSqrtWord32(val)
 *      Returns floor(sqrt(val)) as a Word16 integer (not a Q-format
 *      fraction).  The upstream helper is documented as "equivalent of
 *      (int)sqrt(val)" with the fractional part discarded.  The plain-C
 *      implementation uses the standard 32-bit integer square-root
 *      algorithm (bit-by-bit restoration) with 64-bit-safe intermediates.
 *      Negative inputs are clamped to 0; results exceeding MAX_16 are
 *      saturated to MAX_16.
 *
 *    getNormReciprocalWord16(x)
 *      Returns the reciprocal 1/x scaled to Q15, i.e. the high 16 bits of
 *      a Q31 reciprocal.  For x > 0:
 *          result = round( (2^31) / x ) >> 16   (clamped to 0x7FFF for x==1)
 *      which matches the upstream BASOP_util_normReciprocal[] table values
 *      to the bit.  For x <= 0 the function returns 0; the FX call sites
 *      always pass a positive filter length.
 *
 *  Implementation notes
 *  --------------------
 *  Only "typedefs.h" is included.  The helpers are written in plain C99
 *  with 64-bit intermediates so they have no dependency on other BASOP
 *  utilities.  This mirrors the existing parent helpers get_gain_fx.c,
 *  lerp_fx.c and basop1616_fx.c.
 * ============================================================================ */

#include "typedefs.h"

#ifndef MAX_16
#define MAX_16 (Word16)0x7fff
#endif
#ifndef MIN_16
#define MIN_16 (Word16)0x8000
#endif

/* ---------------------------------------------------------------------------
 *  Plain-C equivalent of ITU-T G.191 norm_s().
 *
 *  For a non-zero 16-bit value, returns the number of left shifts required
 *  to bring the magnitude into the interval [0x4000, 0x7FFF].  This is
 *  exactly the behavior used by the upstream getScaleFactor16() helper.
 *
 *  The special case x == -1 (0xFFFF) returns 15, matching norm_s() in
 *  basic_op/basop32.c; without this guard the bitwise-complement path
 *  would yield 0 and the value would not be normalized to -0x8000.
 * --------------------------------------------------------------------------- */
static Word16 basop_extra_norm_s(Word16 x)
{
    Word16 v;
    Word16 n;

    if (x == 0)
    {
        return 0;
    }
    if (x == (Word16)0xFFFF)   /* -1: complement is zero, so hard-wire 15 */
    {
        return 15;
    }

    /* For negative values, norm_s() operates on the bitwise complement,
     * which represents the magnitude for two's-complement values. */
    v = (x < 0) ? (Word16)(~x) : x;

    for (n = 0; v < (Word16)0x4000; n++)
    {
        v = (Word16)(v << 1);
    }
    return n;
}

/* ---------------------------------------------------------------------------
 *  getScaleFactor16
 *
 *  Returns the headroom in [0..15] needed to normalize the 16-bit array x.
 *  The upstream definition scans for the largest positive and most-negative
 *  samples, computes the norm_s() headroom for each non-zero extreme, and
 *  returns the smaller of the two headrooms (so the whole array can be
 *  shifted safely).
 * --------------------------------------------------------------------------- */
Word16 getScaleFactor16(const Word16 *x, const Word16 len)
{
    Word16 i;
    Word16 x_max = 0;
    Word16 x_min = 0;
    Word16 i_max = 16;
    Word16 i_min = 16;
    Word16 result;

    for (i = 0; i < len; i++)
    {
        Word16 xi = x[i];
        if (xi >= 0)
        {
            if (xi > x_max)
            {
                x_max = xi;
            }
        }
        else
        {
            if (xi < x_min)
            {
                x_min = xi;
            }
        }
    }

    if (x_max != 0)
    {
        i_max = basop_extra_norm_s(x_max);
    }
    if (x_min != 0)
    {
        i_min = basop_extra_norm_s(x_min);
    }

    result = (i_max < i_min) ? i_max : i_min;
    return (Word16)(result & 0xF);
}

/* ---------------------------------------------------------------------------
 *  getSqrtWord32
 *
 *  Returns floor(sqrt(val)) as a Word16.  The upstream helper is documented
 *  as returning the integer part of the square root (fractional part
 *  discarded).  Negative inputs are treated as zero; results above MAX_16
 *  are saturated.  The bit-by-bit restoration algorithm below is the
 *  standard 32-bit integer square root and is exact for all non-negative
 *  inputs.
 * --------------------------------------------------------------------------- */
Word16 getSqrtWord32(Word32 val)
{
    UWord32 v;
    UWord32 r;
    UWord32 bit;
    Word16 result;

    if (val <= 0)
    {
        return 0;
    }

    v = (UWord32)val;
    r = 0;

    /* Start with the highest power of four not exceeding v. */
    bit = 1UL << 30;
    while (bit > v)
    {
        bit >>= 2;
    }

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

    result = (Word16)r;
    if (r > (UWord32)(Word16)MAX_16)
    {
        result = MAX_16;
    }
    return result;
}

/* ---------------------------------------------------------------------------
 *  getNormReciprocalWord16
 *
 *  Returns 1/x in Q15 (the high 16 bits of a Q31 reciprocal).  The formula
 *  below uses 64-bit arithmetic with round-to-nearest so that it matches the
 *  upstream BASOP_util_normReciprocal[] table values exactly for all
 *  positive x:
 *
 *      q = round(2^31 / x)  clamped to 0x7FFFFFFF
 *      result = q >> 16
 *
 *  For x <= 0 the function returns 0.  The FX call sites always pass a
 *  positive out_filt_length.
 * --------------------------------------------------------------------------- */
Word16 getNormReciprocalWord16(Word16 x)
{
    unsigned long long q;

    if (x <= 0)
    {
        return 0;
    }

    /* Round(0x80000000 / x) computed as (2*0x7FFFFFFF + x) / (2*x).
     * The intermediate uses 64 bits so it cannot overflow. */
    q = (0xFFFFFFFFULL + (unsigned long long)(UWord16)x)
        / (2ULL * (unsigned long long)(UWord16)x);

    /* Clamp to the largest positive Q31 value; this only affects x == 1. */
    if (q > 0x7FFFFFFFULL)
    {
        q = 0x7FFFFFFFULL;
    }

    return (Word16)(q >> 16);
}


/* ============================================================================
 *  Batch B: additional BASOP utility helpers
 *
 *  The implementations below are plain-C equivalents of the upstream IVAS
 *  BASOP helpers referenced by the vendored 3GPP EVS FX sources.  They use
 *  only the types from typedefs.h plus local 64-bit intermediate types, and
 *  deliberately do not pull in the float prot.h / cnst.h / typedef.h chain.
 * ============================================================================ */

#ifndef MAX_32
#define MAX_32 (Word32)0x7fffffffL
#endif
#ifndef MIN_32
#define MIN_32 (Word32)0x80000000L
#endif

/*
 *  Note: typedefs.h only defines Word40; we deliberately do NOT introduce
 *  local Word64 / UWord64 typedefs because the FX shim pulls in enhUL32.h,
 *  which #defines UWord64 as "unsigned long long".  Using the same name
 *  would macro-expand the typedef into invalid syntax.  The helpers below
 *  spell out "long long" / "unsigned long long" explicitly.
 */

/* ---------------------------------------------------------------------------
 *  Plain-C equivalent of ITU-T G.191 norm_l().
 *
 *  For a non-zero 32-bit value, returns the number of left shifts required
 *  to bring the magnitude into the interval [0x40000000, 0x7FFFFFFF].
 *  The special cases 0 and 0xFFFFFFFF follow the G.191 definitions.
 * --------------------------------------------------------------------------- */
static Word16 basop_extra_norm_l(Word32 x)
{
    UWord32 v;
    Word16 n;

    if (x == 0)
    {
        return 0;
    }
    if (x == (Word32)0xFFFFFFFFL)
    {
        return 31;
    }

    v = (x < 0) ? (UWord32)(~x) : (UWord32)x;

    for (n = 0; v < (UWord32)0x40000000UL; n++)
    {
        v <<= 1;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 *  Saturating 32-bit left shift (L_shl equivalent).  Negative shift counts
 *  shift right arithmetically.  The shift is performed on the unsigned
 *  32-bit pattern to avoid signed left-shift UB; saturation is decided from
 *  the operand sign/magnitude before the value is reinterpreted.
 * --------------------------------------------------------------------------- */
static Word32 basop_extra_l_shl(Word32 x, Word16 shift)
{
    unsigned long long mag;

    if (shift == 0)
    {
        return x;
    }
    if (shift > 0)
    {
        if (shift > 31)
        {
            shift = 31;
        }
        if (x >= 0)
        {
            mag = (unsigned long long)(UWord32)x;
            if (mag > ((unsigned long long)MAX_32 >> shift))
            {
                return MAX_32;
            }
        }
        else
        {
            mag = (unsigned long long)(-(UWord32)x);
            if (mag > (1ULL << (31 - shift)))
            {
                return MIN_32;
            }
        }
        return (Word32)((unsigned long long)(UWord32)x << shift);
    }
    /* shift < 0: arithmetic right shift (MSVC uses arithmetic shifts for
     * signed integers; the values here are all magnitude-normalized so the
     * result is well-defined in practice). */
    return (Word32)(x >> (-shift));
}

/* ---------------------------------------------------------------------------
 *  Absolute value with MIN_32 saturation (L_abs equivalent).
 * --------------------------------------------------------------------------- */
static Word32 basop_extra_l_abs(Word32 x)
{
    if (x == MIN_32)
    {
        return MAX_32;
    }
    return (x < 0) ? -x : x;
}

/* ---------------------------------------------------------------------------
 *  Round a Q31 value to the nearest Q15/16 boundary (round_fx equivalent).
 *  Returns (x + 0x8000) >> 16 with 32-bit saturation.
 * --------------------------------------------------------------------------- */
static Word16 basop_extra_round_fx(Word32 x)
{
    long long v;

    v = (long long)x + 0x8000LL;

    if (v > (long long)MAX_32)
    {
        return (Word16)(MAX_32 >> 16);
    }
    if (v < (long long)MIN_32)
    {
        return (Word16)(MIN_32 >> 16);
    }
    return (Word16)(v >> 16);
}

/* ---------------------------------------------------------------------------
 *  Fractional division Q15 = (num / den) * 2^15, truncating toward zero.
 *  Both inputs are positive; den must be normalized to [0x4000, 0x7FFF].
 *  This mirrors the upstream G.191 div_s() behaviour: the division floors
 *  the fractional result, and num == den saturates to MAX_16.
 * --------------------------------------------------------------------------- */
static Word16 basop_extra_div_s(Word16 num, Word16 den)
{
    long long q;

    q = ((long long)num << 15) / den;

    if (q >= (long long)MAX_16)
    {
        return MAX_16;
    }
    return (Word16)q;
}

/* ---------------------------------------------------------------------------
 *  Saturate a 64-bit signed accumulator to Word32 (W_sat_l equivalent).
 * --------------------------------------------------------------------------- */
static Word32 basop_extra_sat_l(long long v)
{
    if (v > (long long)MAX_32)
    {
        return MAX_32;
    }
    if (v < (long long)MIN_32)
    {
        return MIN_32;
    }
    return (Word32)v;
}

/* ---------------------------------------------------------------------------
 *  Normalize a 64-bit signed accumulator to Q31 and return the binary
 *  exponent.  This is a conservative plain-C equivalent of the upstream
 *  w_norm_llQ31() helper used by Dot_productSq16HQ.
 *
 *  Returns a Word32 in Q31 (magnitude in [0x40000000, 0x7FFFFFFF]) and
 *  stores in *exp the power-of-two correction such that, when the
 *  accumulator is interpreted as a Q0 integer,
 *
 *      accumulator_value = result * 2^(*exp - 31)
 *
 *  approximately.  The caller is responsible for adjusting *exp for the
 *  actual Q format of the accumulated data.
 * --------------------------------------------------------------------------- */
static Word32 basop_extra_norm_ll_q31(long long acc, Word16 *exp)
{
    Word16 sign;
    Word16 p;
    Word16 shift;
    unsigned long long uacc;

    if (acc == 0)
    {
        *exp = 0;
        return 0;
    }

    /* Safe magnitude extraction that also works for LLONG_MIN. */
    sign = (acc < 0) ? 1 : 0;
    uacc = (unsigned long long)acc;
    if (sign)
    {
        uacc = ~uacc + 1ULL;
    }

    /* Find the position p of the highest set bit (0-indexed from LSB). */
    p = 0;
    {
        unsigned long long t = uacc;
        while (t > 1ULL)
        {
            t >>= 1;
            p++;
        }
    }

    /* Normalize to Q31: bring bit p to bit 30. */
    shift = (Word16)(30 - p);

    if (shift >= 0)
    {
        uacc <<= shift;
    }
    else
    {
        uacc >>= (-shift);
    }

    /*
     *  Return the exponent for the raw-normalized Q31 convention used by
     *  upstream Dot_productSq16HQ / frame_ener_fx: the returned Word32 is
     *  treated as an integer in [0x40000000,0x7FFFFFFF] and
     *      accumulator_value = result * 2^(*exp - 31)
     *  hence *exp = 31 - shift (e.g. accumulator == 1 gives exp == 1).
     */
    *exp = (Word16)(31 - shift);

    /* Clamp to the Q31 range and re-apply the sign. */
    if (!sign)
    {
        if (uacc > (unsigned long long)MAX_32)
        {
            uacc = (unsigned long long)MAX_32;
        }
        return (Word32)uacc;
    }
    else
    {
        Word32 result;
        if (uacc > ((unsigned long long)MAX_32 + 1ULL))
        {
            uacc = (unsigned long long)MAX_32 + 1ULL;
        }
        result = (Word32)(-(long long)uacc);
        if (result > 0)
        {
            result = MIN_32;
        }
        return result;
    }
}

/* ---------------------------------------------------------------------------
 *  BASOP_Util_Divide3232_Scale
 *
 *  Returns the fractional ratio x / y as a Q15 Word16 together with a scale
 *  factor *s such that
 *
 *      x / y  ~=  z * 2^(*s - 15)
 *
 *  The algorithm follows the upstream reference: normalize the 32-bit
 *  denominator so that its high 16 bits can be passed to the 32/16 divide
 *  helper, accumulate the normalization shifts into *s, and perform the
 *  final fractional division with the sign of the operands.
 *
 *  y == 0 is handled defensively by returning 0 and *s == 0; the FX call
 *  sites explicitly avoid a zero denominator.
 * --------------------------------------------------------------------------- */
Word16 BASOP_Util_Divide3232_Scale(Word32 x, Word32 y, Word16 *s)
{
    Word32 ax;
    Word32 ay;
    Word16 sx;
    Word16 sy;
    Word16 sy2;
    Word16 dy;
    Word16 z;
    Word16 sign;

    if (x == 0 || y == 0)
    {
        *s = 0;
        return 0;
    }

    sign = (Word16)(((x ^ y) < 0) ? 1 : 0);

    ax = basop_extra_l_abs(x);
    ay = basop_extra_l_abs(y);

    /* Normalize numerator to [0x20000000, 0x3FFFFFFF] (sx = norm_l(ax) - 1). */
    sx = (Word16)(basop_extra_norm_l(ax) - 1);
    ax = basop_extra_l_shl(ax, sx);

    /* Normalize denominator: sy = max(0, norm_l(ay) - 1). */
    sy = basop_extra_norm_l(ay);
    if (sy > 0)
    {
        sy = (Word16)(sy - 1);
    }
    ay = basop_extra_l_shl(ay, sy);

    /* Use the high 16 bits of the normalized denominator. */
    dy = (Word16)(ay >> 16);

    /* Normalize the 16-bit denominator to [0x4000, 0x7FFF]. */
    sy2 = basop_extra_norm_s(dy);
    dy = (Word16)(dy << sy2);

    /* Fractional divide.  round_fx(ax) is in [0x2000, 0x3FFF], dy in
     * [0x4000, 0x7FFF], so the result is a Q15 value in [0, 0x7FFF]. */
    z = basop_extra_div_s(basop_extra_round_fx(ax), dy);

    *s = (Word16)(sy + sy2 - sx);

    if (sign)
    {
        z = (Word16)(-z);
    }

    return z;
}

/* ---------------------------------------------------------------------------
 *  Dot_productSq16HQ
 *
 *  Computes L_off + sum(x[i] * x[i]) using a 64-bit accumulator and the
 *  fractional (x2) multiply semantics used by the upstream W_mac_16_16
 *  operator, then normalizes the result to Q31 with its binary exponent.
 *
 *  The caller must adjust *exp for the actual Q format of x[] (the
 *  reference call sites subtract 2*Q_syn and an extra 1 to compensate the
 *  implicit left shift of the fractional mac operation).
 * --------------------------------------------------------------------------- */
Word32 Dot_productSq16HQ(const Word32 L_off, const Word16 x[], const Word16 lg, Word16 *exp)
{
    Word16 i;
    long long acc;
    Word32 prod;

    acc = (long long)L_off;

    for (i = 0; i < lg; i++)
    {
        Word16 xi = x[i];
        prod = (Word32)xi * (Word32)xi;

        /* Fractional multiply: shift left by 1 with 32-bit saturation
         * (matches W_mac_16_16 / L_mac semantics). */
        {
            long long p64 = (long long)prod << 1;
            if (p64 > (long long)MAX_32)
            {
                p64 = (long long)MAX_32;
            }
            else if (p64 < (long long)MIN_32)
            {
                p64 = (long long)MIN_32;
            }
            acc += p64;
        }
    }

    return basop_extra_norm_ll_q31(acc, exp);
}

/* ---------------------------------------------------------------------------
 *  Dot_product12_offs
 *
 *  Computes L_off + sum(x[i] * y[i]) using a 64-bit accumulator and the
 *  non-fractional (x1) multiply semantics used by the upstream
 *  W_mac0_16_16 operator, saturates to 32 bits, and normalizes the result
 *  to Q31 with its binary exponent.
 * --------------------------------------------------------------------------- */
Word32 Dot_product12_offs(const Word16 x[], const Word16 y[], const Word16 lg, Word16 *exp, Word32 L_off)
{
    Word16 i;
    long long acc;
    Word32 L_sum;
    Word16 sft;

    acc = (long long)L_off;

    for (i = 0; i < lg; i++)
    {
        acc += (long long)x[i] * (long long)y[i];
    }

    L_sum = basop_extra_sat_l(acc);

    sft = basop_extra_norm_l(L_sum);
    if (sft > 0)
    {
        L_sum = basop_extra_l_shl(L_sum, sft);
    }

    if (L_sum != 0)
    {
        sft = (Word16)(31 - sft);
    }

    if (exp != NULL)
    {
        *exp = sft;
    }

    return L_sum;
}

/* ============================================================================
 *  Batch C: tiny FX linkage helpers
 *
 *  These three symbols are referenced by vendored 3GPP EVS FX sources but have
 *  no implementation in the vendored snapshot (or their macro definition is
 *  hidden by include-order collisions).  They are deliberately small and
 *  self-contained so they can live next to the other BASOP extras.
 * ============================================================================ */

/* ---------------------------------------------------------------------------
 *  cast16
 *
 *  In the upstream 3GPP EVS reference, `cast16` is a macro in
 *  lib_com/move.h that maps to the no-op data-move helper `move16()`.
 *  In this parent-repo build the include path picks basic_op/move.h first,
 *  which defines `move16()` but does *not* define the `cast16` alias, so
 *  `cast16();` calls in vendored sources (e.g. lib_dec/avq_dec_fx.c:229)
 *  remain unresolved function calls.  Provide the same no-op wrapper here.
 * --------------------------------------------------------------------------- */
void cast16(void)
{
}

/* ---------------------------------------------------------------------------
 *  bufferCopyFx
 *
 *  Called once in the vendored snapshot:
 *      lib_dec/updt_dec_fx.c:603
 *          bufferCopyFx( synth + L_frame - L_frame/2,
 *                        st->old_syn_Overl,
 *                        L_frame/2,
 *                        0, -1, 0, 0 );
 *
 *  The upstream fixed-point helper copies a 16-bit buffer while converting
 *  between Q formats.  The parameters are the source pointer, destination
 *  pointer, length, source Q format, previous destination Q format, a flags
 *  word, and the desired destination Q format.  For the single call site the
 *  source and destination Q formats coincide (Q0), so the operation reduces
 *  to a plain Copy().  The implementation below handles arbitrary shifts
 *  safely for any future callers.
 * --------------------------------------------------------------------------- */
void bufferCopyFx(
    const Word16 *src,
    Word16 *dst,
    Word16 len,
    Word16 Qf_src,
    Word16 Qf_old_xnq,
    Word16 f,
    Word16 Qf_dst_new)
{
    Word16 i;
    Word16 shift;

    /* The existing call site does not need the old destination Q format or
     * the flags word; mark them unused to keep compilers quiet. */
    (void)Qf_old_xnq;
    (void)f;

    shift = (Word16)(Qf_dst_new - Qf_src);

    if (shift == 0)
    {
        for (i = 0; i < len; i++)
        {
            dst[i] = src[i];
        }
    }
    else if (shift < 0)
    {
        for (i = 0; i < len; i++)
        {
            dst[i] = (Word16)(src[i] >> (-shift));
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            Word32 v = (Word32)src[i] << shift;
            if (v > (Word32)MAX_16)
            {
                v = (Word32)MAX_16;
            }
            else if (v < (Word32)MIN_16)
            {
                v = (Word32)MIN_16;
            }
            dst[i] = (Word16)v;
        }
    }
}

/* ---------------------------------------------------------------------------
 *  getInvFrameLen
 *
 *  Declared in external/3gpp-evs/lib_com/prot_fx.h:
 *      Word16 getInvFrameLen(Word16 L_frame); // returns 1/L_frame in Q21
 *
 *  Returns the reciprocal of the frame length scaled by 2^21:
 *      result = round( (1 << 21) / L_frame )
 *  clamped to MAX_16.  The 64-bit intermediate avoids overflow for any
 *  positive L_frame.
 * --------------------------------------------------------------------------- */
Word16 getInvFrameLen(Word16 L_frame)
{
    unsigned long long q;

    if (L_frame <= 0)
    {
        return 0;
    }

    q = ((1ULL << 21) + (unsigned long long)(UWord16)(L_frame >> 1))
        / (unsigned long long)(UWord16)L_frame;

    if (q > (unsigned long long)MAX_16)
    {
        q = (unsigned long long)MAX_16;
    }

    return (Word16)q;
}
