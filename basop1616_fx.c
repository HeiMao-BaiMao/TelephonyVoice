/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Provides four fixed-point arithmetic primitives that are *called* by the
 *  vendored 3GPP EVS FX tree under
 *      external/3gpp-evs/
 *  but whose implementations are NOT shipped anywhere in that vendored
 *  tree.  They appear in source files without any matching header
 *  declaration in `prot_fx.h` and without any `basop_*` source file:
 *
 *      Word16 idiv1616 (Word16 x, Word16 y);
 *      Word16 imult1616(Word16 x, Word16 y);
 *      Word16 divide1616(Word16 x, Word16 y);
 *      Word16 divide3232(Word32 x, Word32 y);
 *
 *  Upstream equivalent definitions exist only in the float reference tree
 *  (e.g. as `idiv1616U` / `divide_*` in `basop_util.c` for the unsigned
 *  variant, or in older 3GPP revisions that pre-date this vendored
 *  snapshot).  The read-only submodule does not contain the signed
 *  versions used here, so we provide them as a parent-side C translation
 *  unit and wire it through `EVS_FX_EXTRAS_LIB_COM` in
 *  `cmake/3gpp-evs.cmake` so they end up in `evs-lib-com-fx.lib` (the
 *  common FX support library that both `evs-lib-enc-fx` and
 *  `evs-lib-dec-fx` PUBLIC-link against).
 *
 *  Key call sites (all read-only, vendored 3GPP source):
 *    idiv1616
 *      lib_com/modif_fs_fx.c:131
 *          lg_out = idiv1616(i_mult2(lg, fac_num), fac_den);
 *      lib_dec/evs_dec_fx.c:550, 565, 580
 *          incr = idiv1616(L_FRAME*2, add(shl(tmps,1),1));
 *      lib_dec/core_switching_dec_fx.c:402
 *          idiv1616(sub(add(delay_comp, ...->no_channels),1),
 *                   ...->no_channels)
 *    imult1616
 *      lib_com/bitstream_fx.c:960
 *          st->core_fx = imult1616(get_next_indice_fx(st, 1), HQ_CORE);
 *      lib_enc/pre_proc_fx.c:636, 796
 *          shr(imult1616(st->L_frame_fx, 9), 4);
 *    divide1616
 *      lib_com/lpc_tools_fx.c:713, 725  (spec2isf, used as Q15 ratio)
 *    divide3232
 *      lib_com/lpc_tools_fx.c:625, 649  (E_LPC_schur)
 *      lib_com/est_tilt_fx.c:240        (BASOP_SATURATE_WARNING_OFF / ON
 *                                         bracket signals saturation is
 *                                         expected)
 *
 *  Why this parent-side helper exists
 *  ----------------------------------
 *  The vendored 3GPP EVS FX tree under `external/3gpp-evs/` is read-only
 *  in this repo (it is a pinned submodule reference).  The four symbols
 *  above are referenced by the FX encoder and decoder .c sources but
 *  have neither a definition nor a prototype anywhere in that tree.
 *  Without these helpers, the FX link fails with `LNK2019` /
 *  `LNK2001` unresolved symbols for `idiv1616` / `imult1616` /
 *  `divide1616` / `divide3232`.  The same FX typedef shim that hides the
 *  float `typedef.h` / `cnst.h` bodies from FX sources (and provides
 *  `Word16` / `Word32` / `MAX_16` / `MIN_16`) is force-included into
 *  every FX translation unit, including this one, so we get the same
 *  fixed-point type set here.
 *
 *  Numerical contract
 *  ------------------
 *    idiv1616(x, y):  signed 16/16 -> 16 integer division, truncating
 *                     toward zero.  Mirrors C `(int)x / (int)y` semantics
 *                     for the FX call sites, with saturation:
 *                       y == 0           ->  saturate to MAX_16 or
 *                                            MIN_16 according to the
 *                                            sign of x (quotient sign).
 *                       |result| > 32767 ->  saturate to MAX_16 /
 *                                            MIN_16 preserving sign.
 *                     Current call sites only exercise the positive
 *                     branch, but the implementation is sign-defensive.
 *
 *    imult1616(x, y): signed 16 * 16 -> 16 multiply with saturation.
 *                     `x*y` in 32 bits may exceed the Word16 range; the
 *                     function returns the saturated result
 *                     (MAX_16 / MIN_16 preserving sign of the exact
 *                     product).  Mirrors ITU-T G.191's saturating
 *                     16-bit basic-operator multiply.
 *
 *    divide1616(x, y):
 *                     signed fractional ratio in Q15:
 *                         result = sat( (|x| << 15) / |y| )  * sign(x) * sign(y)
 *                     i.e. Q15 of x / y, with sign re-applied, with
 *                     saturation to MAX_16 / MIN_16 preserving sign.
 *                     y == 0 -> saturate to MAX_16 / MIN_16 according to
 *                     the quotient sign (sign(x) * sign(y)).  Used by
 *                     `spec2isf()` in lib_com/lpc_tools_fx.c to compute
 *                     a fractional root position between two adjacent
 *                     spectral samples (the per-step formula
 *                     `tmp` is then `shr(tmp, 8)` to convert Q15 -> Q8
 *                     and added to `shl(sub(specix,1),7)` to form the
 *                     final LSF).  A Q15 input ratio is the right
 *                     contract.
 *
 *    divide3232(x, y): same as divide1616 but with Word32 inputs and a
 *                     64-bit intermediate, so the (|x| << 15) shift
 *                     never overflows.  Used by `E_LPC_schur()` and
 *                     `est_tilt()`; the call sites that wrap it in
 *                     `BASOP_SATURATE_WARNING_OFF` / `_ON` (e.g.
 *                     est_tilt_fx.c:239,242) explicitly expect a
 *                     saturating Q15 result.
 *
 *  Implementation notes
 *  --------------------
 *  We do NOT rely on any of the other basic-operator helpers
 *  (add/sub/shl/shr/div_s/etc.) - those are defined in the FX lib but
 *  introduce a basop.h dependency that complicates the build graph for
 *  what is otherwise a tiny, low-risk helper.  Plain C99 with
 *  `long long` / `unsigned long long` is sufficient:
 *
 *    |x * y|      <= 32767 * 32767  < 2^30          (imult1616)
 *    |x| << 15    <= 32767 << 15    < 2^30          (divide1616)
 *    |x| (Word32) <= 2^31 - 1                       (divide3232)
 *    (|x| << 15) (64-bit)                           (divide3232)
 *
 *  All fit comfortably in `long long`.  Local MAX_16 / MIN_16 constants
 *  are defined here (the FX typedef shim already defines the same
 *  names from basic_op/basop32.h:69-70, so this is just defensive
 *  belt-and-braces if the shim ever changes).
 * ============================================================================ */

#include "typedefs.h"

/* MAX_16 / MIN_16 are normally visible via the FX typedef shim (which
 * pulls in basic_op/typedefs.h -> basop32.h).  Define them locally too
 * so this TU compiles even if the shim's include order changes. */
#ifndef MAX_16
#define MAX_16 (Word16)0x7fff
#endif
#ifndef MIN_16
#define MIN_16 (Word16)0x8000
#endif

/* ---------------------------------------------------------------------------
 *  idiv1616
 *
 *  Signed integer division `x / y`, truncating toward zero, with the
 *  result saturated to the Word16 range.
 *
 *  This is NOT the upstream unsigned `idiv1616U` (which assumes
 *  0 <= x < y, normalizes y > x, then runs `div_s` for 15 iterations
 *  to get a Q15 fraction, then shifts back to integer).  The signed
 *  version here is a straight C-style integer division because the FX
 *  call sites exercise it with two small positive operands (e.g.
 *  `idiv1616(L_FRAME*2, add(shl(tmps,1),1))` where tmps is a small
 *  integer, and `idiv1616(i_mult2(lg, fac_num), fac_den)` where
 *  fac_num / fac_den are ratio constants in lib_com/modif_fs_fx.c).
 *  Both forms stay well within the [-32768, 32767] Word16 range, so the
 *  branch + saturate is purely defensive.
 *
 *  C `long long` is used so we never have to reason about overflow
 *  inside the conversion of the dividend; `llabs` keeps the abs() in
 *  the same 64-bit signed type as the divide.
 * --------------------------------------------------------------------------- */
Word16 idiv1616(Word16 x, Word16 y)
{
    long long xs = (long long)x;
    long long ys = (long long)y;
    long long q;

    /* y == 0: saturate to the signed Word16 extreme whose sign matches
     * the quotient sign.  Quotient sign is sign(x) * sign(y); when
     * y == 0 the convention here is to return the same signed extreme
     * that a saturating `x / y` would produce if y were a tiny epsilon
     * of the same sign as the actual y, i.e. sign(x) * sign(y).  When
     * y == 0 we still take sign(x) because that matches "abs(x) / 0"
     * blowing up to +infinity if x > 0 and -infinity if x < 0 (and
     * the call sites always pass positive x). */
    if (y == 0)
    {
        if (x >= 0)
        {
            return MAX_16;
        }
        return MIN_16;
    }

    /* Truncate toward zero.  This is exactly what C `(int)x / (int)y`
     * does for two's-complement Word16 operands; the (long long) cast
     * keeps the divide in 64-bit so no intermediate can overflow. */
    q = xs / ys;

    /* Saturate to Word16. */
    if (q > 32767LL)
    {
        return MAX_16;
    }
    if (q < -32768LL)
    {
        return MIN_16;
    }
    return (Word16)q;
}

/* ---------------------------------------------------------------------------
 *  imult1616
 *
 *  Signed 16 * 16 -> 16 multiply with saturation.  Mirrors the
 *  saturating-multiply contract used throughout the 3GPP basop
 *  helpers (L_mult / mult_r etc.): the exact 32-bit product is
 *  computed in `long long`, then clamped to the Word16 range with the
 *  sign of the exact product preserved.
 * --------------------------------------------------------------------------- */
Word16 imult1616(Word16 x, Word16 y)
{
    long long p;

    p = (long long)x * (long long)y;

    if (p > 32767LL)
    {
        return MAX_16;
    }
    if (p < -32768LL)
    {
        return MIN_16;
    }
    return (Word16)p;
}

/* ---------------------------------------------------------------------------
 *  divide1616
 *
 *  Signed Q15 fractional ratio `x / y`, with the result saturated to
 *  the Word16 range.  Implements
 *      result = sat( (|x| << 15) / |y| ) * sign(x) * sign(y)
 *  using 64-bit intermediates so no truncation happens before the
 *  Q15 shift.  Used by `spec2isf()` in lib_com/lpc_tools_fx.c to
 *  compute a fractional root position between two adjacent spectral
 *  samples; a Q15 ratio is the right contract because the per-step
 *  formula then `shr(tmp, 8)`s to Q8 and adds it to a `shl(.., 7)`
 *  base offset to form the final LSF.
 *
 *  y == 0 -> saturate to the signed Word16 extreme whose sign matches
 *  the quotient sign (sign(x) * sign(y)).  When y == 0 AND x == 0 the
 *  exact quotient is undefined; we return MAX_16 (positive saturation
 *  is the conservative "blow up to +infinity" choice, consistent with
 *  the call sites that only call this with at least one non-zero
 *  argument in practice).
 * --------------------------------------------------------------------------- */
Word16 divide1616(Word16 x, Word16 y)
{
    long long abs_x;
    long long abs_y;
    long long q_abs;
    long long q;
    Word16 sign_x;
    Word16 sign_y;
    Word16 quotient_sign;     /* +1 or -1 */

    sign_x = (x < 0) ? (Word16)-1 : (Word16)+1;
    sign_y = (y < 0) ? (Word16)-1 : (Word16)+1;
    quotient_sign = (Word16)((sign_x == sign_y) ? +1 : -1);

    /* y == 0: saturate to the signed Word16 extreme matching the
     * quotient sign. */
    if (y == 0)
    {
        if (quotient_sign > 0)
        {
            return MAX_16;
        }
        return MIN_16;
    }

    /* 64-bit intermediates keep the (|x| << 15) shift safe even if a
     * future call site passes a negative abs that's at the Word16
     * extreme (|x| <= 32767, so |x| << 15 <= 1073709056, well below
     * 2^31). */
    abs_x = (x < 0) ? -((long long)x) : (long long)x;
    abs_y = (y < 0) ? -((long long)y) : (long long)y;
    q_abs = (abs_x << 15) / abs_y;

    /* Saturate the magnitude to the positive Word16 extreme, then
     * re-apply the quotient sign.  This matches the saturating-Q15
     * contract used by the upstream basop util helpers (and the
     * `BASOP_SATURATE_WARNING_OFF` / `_ON` brackets at the call sites
     * that consume the result, e.g. est_tilt_fx.c:239,242). */
    if (q_abs > 32767LL)
    {
        q_abs = 32767LL;
    }
    q = q_abs * (long long)quotient_sign;

    if (q > 32767LL)
    {
        return MAX_16;
    }
    if (q < -32768LL)
    {
        return MIN_16;
    }
    return (Word16)q;
}

/* ---------------------------------------------------------------------------
 *  divide3232
 *
 *  Same Q15 fractional ratio as `divide1616` but with Word32 inputs
 *  and a 64-bit intermediate, so the (|x| << 15) shift never overflows
 *  even when |x| is close to the Word32 extreme.  Used by
 *  `E_LPC_schur()` in lib_com/lpc_tools_fx.c and by `est_tilt()` in
 *  lib_com/est_tilt_fx.c; the latter wraps the call in
 *  `BASOP_SATURATE_WARNING_OFF` / `_ON` (lines 239, 242) which
 *  explicitly expects a saturating Q15 result.
 * --------------------------------------------------------------------------- */
Word16 divide3232(Word32 x, Word32 y)
{
    long long abs_x;
    long long abs_y;
    long long q_abs;
    long long q;
    Word16 sign_x;
    Word16 sign_y;
    Word16 quotient_sign;     /* +1 or -1 */

    sign_x = (x < 0) ? (Word16)-1 : (Word16)+1;
    sign_y = (y < 0) ? (Word16)-1 : (Word16)+1;
    quotient_sign = (Word16)((sign_x == sign_y) ? +1 : -1);

    /* y == 0: saturate to the signed Word16 extreme matching the
     * quotient sign (same convention as divide1616). */
    if (y == 0)
    {
        if (quotient_sign > 0)
        {
            return MAX_16;
        }
        return MIN_16;
    }

    /* 64-bit intermediates keep the (|x| << 15) shift safe: even at
     * |x| = 2^31 - 1 we have |x| << 15 < 2^46, well within long long. */
    abs_x = (x < 0) ? -((long long)x) : (long long)x;
    abs_y = (y < 0) ? -((long long)y) : (long long)y;
    q_abs = (abs_x << 15) / abs_y;

    /* Saturate the magnitude to the positive Word16 extreme, then
     * re-apply the quotient sign. */
    if (q_abs > 32767LL)
    {
        q_abs = 32767LL;
    }
    q = q_abs * (long long)quotient_sign;

    if (q > 32767LL)
    {
        return MAX_16;
    }
    if (q < -32768LL)
    {
        return MIN_16;
    }
    return (Word16)q;
}