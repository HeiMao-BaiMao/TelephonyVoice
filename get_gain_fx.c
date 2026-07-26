/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Mirrors the float reference `get_gain()` declared in
 *      external/3gpp-evs/lib_com/get_gain.c
 *  but adapted to the fixed-point (FX) calling convention used by the
 *  FX encoder/decoder:
 *
 *      Word32 get_gain(Word16 x[], Word16 y[], Word16 n);
 *
 *  Why this parent-side helper exists
 *  ----------------------------------
 *  The upstream float implementation lives at
 *      external/3gpp-evs/lib_com/get_gain.c
 *  and has the ABI
 *
 *      float get_gain(float x[], float y[], int n, float *en_y);
 *
 *  Three things make it unusable in our FX static libraries:
 *
 *    1. Signature: 4 args (the optional `en_y` out-param) with
 *       `float` arrays.  The FX call sites in
 *          lib_com/cb_shape_fx.c:110
 *              tilt = extract_l(L_shr(get_gain(buff+M+1, buff+M,
 *                                             L_SUBFR-1), 1));
 *          lib_dec/FEC_scale_syn_fx.c:221
 *          lib_dec/FEC_scale_syn_fx.c:363
 *              tilt = extract_h(L_shl(get_gain(h1+1, h1,
 *                                              L_FRAME/2-1), 15));
 *       pass 3 args with `Word16` arrays and consume the return as
 *       `Word32` (Q16 - `L_shr(..,1)` and `L_shl(..,15)` are Word32
 *       shifts).  Binding the float symbol would either fail to link
 *       or, worse, silently link with a wrong-ABI helper.
 *
 *    2. Includes:  `get_gain.c` pulls in `options.h` + `prot.h` to
 *       get its declaration, both of which are part of the float
 *       reference and are deliberately not built in the FX tree
 *       (the FX typedef shim pre-defines the float cnst/typedef
 *       guards to keep float headers from being processed).
 *
 *    3. Edit policy: the `external/3gpp-evs/` tree is read-only in this
 *       repo (vendored reference), so we cannot mutate the
 *       signature in place.
 *
 *  We therefore provide a tiny parent-side C translation unit with
 *  the FX signature, attach it to the evs-lib-com-fx target via
 *  EVS_FX_EXTRAS_LIB_COM in cmake/3gpp-evs.cmake, and rely on the
 *  force-included FX typedef shim to provide Word16/Word32.
 *
 *  Numerical contract
 *  ------------------
 *  The float reference computes
 *      corr = sum_i  x[i] * y[i]            // signed
 *      ener = sum_i  y[i] * y[i] + 1e-6f   // >= 1e-6, never zero
 *      return corr / ener
 *  and the call sites treat the return as a Q16 value (L_shr(.,1)
 *  and L_shl(.,15) move it from Q16 to Q15/Q31).  We reproduce that
 *  exactly:
 *
 *      corr_q16 = (corr * 65536) / max(ener, 1)     // sign of corr
 *      saturate to WORD32 positive max (0x7FFFFFFFL)
 *      reapply the sign of corr
 *
 *  `max(ener, 1)` is the integer equivalent of the float epsilon
 *  floor (the float `1e-6f` is below 1.0, so on the integer side
 *  we just refuse to divide by zero - any non-zero ener already
 *  vastly exceeds the float epsilon).
 *
 *  Long long / unsigned long long are used internally so we do not
 *  have to think about Word32 overflow inside the loop:
 *      |x[i] * y[i]|  <= 32767 * 32767  < 2^30
 *      sum of n such terms for n up to L_FRAME/2-1 = 479 is < 2^39
 *  which is well inside `long long`.  Division by `ener` (>= 1) is
 *  bounded by |corr| and the final Q16 multiply is bounded by
 *  |corr| * 65536, also safe in long long.  The final cast to
 *  Word32 is saturating.
 * ============================================================================ */

#include "typedefs.h"

Word32 get_gain(Word16 x[], Word16 y[], Word16 n)
{
    long long corr = 0;            /* signed accumulator:   sum x[i]*y[i] */
    unsigned long long ener = 0u;  /* unsigned accumulator: sum y[i]*y[i] */
    Word16 i;

    /* One pass: sum products (signed) and squares (unsigned).  Using
     * long long / unsigned long long keeps us out of overflow trouble
     * even at L_FRAME/2 = 160 samples. */
    for (i = 0; i < n; i++)
    {
        corr += (long long)x[i] * (long long)y[i];
        ener += (unsigned long long)y[i] * (unsigned long long)y[i];
    }

    /* Integer analogue of the float `ener + 1e-6f`: refuse to divide
     * by zero.  The float epsilon is far below 1.0, so any non-zero
     * ener already dominates it; the only branch we need is the
     * exact-zero guard. */
    if (ener == 0u)
    {
        ener = 1u;
    }

    /* Q16:  q16 = corr * 65536 / ener  (with abs() inside the divide
     * to keep the division well-defined, sign re-applied below). */
    {
        unsigned long long abs_corr;
        unsigned long long q_abs;
        Word32 result;
        Word16 negative;

        negative = (corr < 0) ? 1 : 0;
        abs_corr = (negative != 0) ? (unsigned long long)(-corr)
                                   : (unsigned long long)corr;

        q_abs = abs_corr * 65536ull / ener;

        /* Saturate to Word32 positive max, matching the float version's
         * "any non-zero ratio is a legal float" semantics.  The Q16
         * value 0x7FFFFFFF is the largest representable positive
         * Word32 (about 32768.0 in Q16).  Values above that are outside
         * the signed Word32 range and must be clamped before the final
         * cast. */
        if (q_abs > 0x7FFFFFFFu)
        {
            q_abs = 0x7FFFFFFFu;
        }

        result = (Word32)q_abs;
        if (negative != 0)
        {
            result = (Word32)(-result);
        }
        return result;
    }
}
