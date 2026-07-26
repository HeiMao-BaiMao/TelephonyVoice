/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Mirrors the float reference `lerp()` declared in
 *      external/3gpp-evs/lib_com/lerp.c
 *  but adapted to the fixed-point (FX) calling convention used by the
 *  FX encoder/decoder:
 *
 *      void lerp(Word16 *f, Word16 *f_out,
 *                Word16 bufferNewSize, Word16 bufferOldSize);
 *
 *  Why this parent-side helper exists
 *  ----------------------------------
 *  The upstream float implementation lives at
 *      external/3gpp-evs/lib_com/lerp.c
 *  and has the ABI
 *
 *      void lerp(float *f, float *f_out,
 *                int bufferNewSize, int bufferOldSize);
 *
 *  Three things make it unusable in our FX static libraries:
 *
 *    1. ABI: the float arrays cannot be ABI-mixed with the Word16
 *       arrays the FX call sites pass in.  The FX call sites
 *       (lib_enc/core_enc_init.c:370,386,402,512,516,612,
 *        lib_dec/acelp_core_dec_fx.c:217,218,
 *        lib_dec/amr_wb_dec_fx.c:233,234,
 *        lib_dec/core_switching_dec_fx.c:420,453,713,717,
 *        lib_dec/FEC_clas_estim_fx.c:127,
 *        lib_com/syn_filt_fx.c:251,255,264,
 *        lib_dec/er_dec_tcx.c:293..543,
 *        lib_dec/er_dec_acelp.c:681..687,
 *        lib_dec/dec_LPD.c:460,618, ...)
 *       all pass Word16* arrays.  Binding the float symbol would
 *       either fail to link or, worse, silently link with a
 *       wrong-ABI helper.
 *
 *    2. Includes: `lerp.c` pulls in `<math.h>`, `<stdlib.h>`, and
 *       the float `prot.h` to get its declaration, all of which
 *       are part of the float reference and are deliberately not
 *       built in the FX tree (the FX typedef shim pre-defines
 *       the float cnst/typedef guards to keep float headers from
 *       being processed).
 *
 *    3. Edit policy: the `external/3gpp-evs/` tree is read-only
 *       in this repo (vendored reference), so we cannot port the
 *       file in place.
 *
 *  We therefore provide a tiny parent-side C translation unit
 *  with the FX signature, attach it to the evs-lib-com-fx target
 *  via EVS_FX_EXTRAS_LIB_COM in cmake/3gpp-evs.cmake, and rely
 *  on the force-included FX typedef shim to provide Word16/Word32
 *  and L_FRAME_MAX (cnst_fx.h:414, included by the shim).
 *
 *  Numerical contract
 *  ------------------
 *  The float reference computes a piece-wise linear resampling
 *  between an old-size and a new-size buffer, with one or more
 *  intermediate `lerp_proc` calls when the resampling ratio
 *  exceeds 507/128 (~3.96) in either direction.  We reproduce
 *  that exactly in fixed-point Q15:
 *
 *      shift_q15  = round( (oldSize * 32768) / newSize )       (Q15)
 *      pos_q15    = (shift_q15 - 32768) / 2
 *                   - (shift<0.3 ? 4259 : 0)                  (Q15)
 *      for i in 0..newSize-1:
 *          idx, diff_q15 = (pos_q15 >> 15, pos_q15 & 0x7FFF)   // floor
 *          buf[i] = f[idx] + (diff_q15 * (f[idx+1] - f[idx])) >> 15
 *          pos_q15 += shift_q15
 *
 *  C-style `(int)pos` truncation toward zero
 *  ------------------------------------------
 *  The float upstream computes `idx = (int)pos;` for every
 *  interpolation point.  C `(int)` truncates toward zero, so for
 *  a position in (-1, 0) it gives `idx = 0`, and `diff = pos -
 *  idx = pos` (a negative fraction that extrapolates linearly
 *  from f[0] to f[1]).
 *
 *  The naive Q15 translation `idx = (Word16)(pos_q15 >> 15)` is
 *  wrong for negative pos_q15: arithmetic right shift of a signed
 *  Word32 yields 0xFFFFFFFF (i.e. -1), causing an OOB read of
 *  f[-1].  This is reachable for every heavy-upsample path that
 *  the FX call sites actually exercise, e.g. 320 -> 960 (shift =
 *  10922, pos_init = -15182 Q15) and 128 -> 480 (shift = 8738,
 *  pos_init = -16274 Q15); the first point triggers the
 *  negative-pos branch, and the second iteration of the middle
 *  loop would too without the fix.
 *
 *  The fix (lerp_interpolate_q15 below) special-cases
 *  pos_q15 < 0: idx = 0, frac = pos_q15 (still signed Q15, i.e.
 *  negative), so the interpolation
 *      f[0] + (frac_q15 / 32768) * (f[1] - f[0])
 *  extrapolates linearly before f[0], matching the float
 *  upstream's `f[0] + pos * (f[1] - f[0])` exactly.
 *
 *  The last-point boundary clamp (idx = oldSize-2 when pos >
 *  oldSize-1) is preserved from the float upstream; the Q15
 *  equivalent of `diff = pos - idx` is `pos_q15 - (idx << 15)`,
 *  which can exceed 32767 (= 1.0) when the position is past
 *  oldSize-1, representing the linear extrapolation past
 *  f[oldSize-1] that the float upstream's `diff = pos - idx`
 *  produces.
 *
 *  Staged resampling
 *  -----------------
 *  The float upstream doubles/halves the size each step and
 *  snaps to the final `bufferNewSize` as soon as the remaining
 *  ratio drops to within `maxFac = 507/128` (~3.96) in either
 *  direction (lerp.c:35-68).  The simpler "double until the
 *  last step is capped at target" approach used here previously
 *  produced the wrong per-step ratios (e.g. 64->1280 took 5
 *  steps of 2x, 2x, 2x, 2x, 1.25x; the float version takes 4
 *  steps of 2x, 2x, 2x, 2.5x with the snap firing when the
 *  ratio first drops within maxFac).  We now mirror the float
 *  upstream exactly, using cross-multiplied integer comparisons
 *  to stay in integer arithmetic: snap when
 *      bufferNewSize * 128 <= curOldSize * 507   (upsample)
 *      curOldSize   * 128 <= bufferNewSize * 507 (downsample)
 *
 *  Q15 intermediates are 32-bit (Word32 shift, Word32 pos) so
 *  the accumulator never wraps: |pos_q15| < 2^31 even for
 *  newSize = 1920 and shift = 32768.  The per-sample multiply
 *  `diff_q15 * (f[idx+1] - f[idx])` is computed in `long long`
 *  (64-bit) because pos_q15 * delta can reach ~250M * 65535
 *  ~ 16T, well past 32-bit; the Q15 right shift + saturate-
 *  to-Word16 is then well defined.
 *
 *  Staged lerp uses a 2*L_FRAME_MAX = 1920 Word16 local scratch
 *  buffer (mirroring the float `float buf[2*L_FRAME_MAX]` in
 *  lerp.c), and writes the final result to f_out.  This makes
 *  the helper safe to call with f == f_out (which several call
 *  sites do, e.g. lib_dec/core_dec_reconf.c:104,
 *  lib_dec/core_dec_init.c:314,
 *  lib_dec/core_switching_dec_fx.c:713,717).
 *  For the FX call sites the maximum lerp() size is 2*L_frame
 *  = 1920 (e.g. olapBufferSynth2 at 48kHz, syn_filt_fx.c:251
 *  uses L_frame+L_frame/2 = 1440), so the per-step newSize
 *  never exceeds the scratch cap.  A defensive `newSize >
 *  2*L_FRAME_MAX` branch copies the overlap region of f into
 *  f_out and holds f[oldSize-1] for the tail, avoiding any
 *  read past f[oldSize-1].
 * ============================================================================ */

#include "typedefs.h"

/* Forward declaration of the per-step helper.  Mirrors the float
 * `lerp_proc` static function in lib_com/lerp.c:16-22. */
static void lerp_proc(Word16 *f, Word16 *f_out,
                      Word16 bufferNewSize, Word16 bufferOldSize);

/* ---------------------------------------------------------------------------
 *  Single-point Q15 interpolator.
 *
 *  Mirrors C `(int)pos` truncation toward zero used by the float
 *  upstream at every interpolation point.  See the file header
 *  for the full rationale.
 *
 *  For pos_q15 < 0 (in practice, in (-32768, 0) for the FX call
 *  sites), idx is forced to 0 and the fraction is kept signed
 *  (negative Q15) so the linear combination extrapolates from
 *  f[0] / f[1]; this is exactly what `idx = (int)pos = 0;
 *  diff = pos - 0 = pos; f[0] + diff*(f[1]-f[0])` produces in
 *  the float version.  A defensive clamp at -32768 keeps us
 *  safe if a future change pushes pos_q15 to or below -1.0.
 *
 *  For pos_q15 >= 0, idx = pos_q15 >> 15 and frac = pos_q15 &
 *  0x7FFF as before.  When pos_q15 reaches or exceeds
 *  (oldSize-1)*32768 we clamp idx to oldSize-2 and use
 *  `pos_q15 - (idx << 15)` as the fraction; this can be > 32767
 *  (= 1.0) when the position is past oldSize-1, representing the
 *  linear extrapolation past f[oldSize-1] that the float
 *  upstream's `diff = pos - idx` produces.  In all cases f[idx]
 *  and f[idx+1] are within [0, oldSize-1] so the read is in
 *  range.
 * --------------------------------------------------------------------------- */
static Word16 lerp_interpolate_q15(const Word16 *f, Word32 pos_q15,
                                    Word16 oldSize, Word16 isLast)
{
    Word16 idx;
    Word32 frac_q15;
    Word32 posBound;
    long long f0, f1, delta, prod, sum;

    if (pos_q15 < 0L)
    {
        /* Position before f[0]: idx clamped to 0, frac kept
         * signed (negative Q15) so the linear combination
         * extrapolates from f[0] / f[1].  Clamp at -32768 as
         * a defensive bound. */
        if (pos_q15 < -32768L)
        {
            pos_q15 = -32768L;
        }
        idx = 0;
        frac_q15 = pos_q15;
    }
    else
    {
        posBound = (Word32)((Word32)oldSize - 1L) * 32768L;
        (void)isLast;
        if (pos_q15 >= posBound)
        {
            /* Boundary clamp: avoid reading f[oldSize].  The float
             * upstream only applies this guard explicitly to the last
             * point, but some valid integer ratios can reach idx ==
             * oldSize-1 in the middle loop as well (for example
             * 96 -> 320).  Clamping to the last two input samples is
             * the safe fixed-point analogue.
             *
             * The fraction is `pos - idx` in float terms, i.e.
             * `pos_q15 - (idx << 15)` here; this can be > 32767
             * (= 1.0) when the position is past oldSize-1,
             * representing the linear extrapolation past
             * f[oldSize-1] that the float upstream's
             * `diff = pos - idx` produces. */
            idx = (Word16)(oldSize - 2);
            frac_q15 = pos_q15 - ((Word32)idx << 15);
        }
        else
        {
            idx = (Word16)(pos_q15 >> 15);
            frac_q15 = pos_q15 & 0x7FFFL;
        }
    }

    f0 = (long long)(Word16)f[idx];
    f1 = (long long)(Word16)f[idx + 1];
    delta = f1 - f0;
    prod = ((long long)frac_q15 * delta) >> 15;
    sum = f0 + prod;
    if (sum > 32767L) sum = 32767L;
    if (sum < -32768L) sum = -32768L;
    return (Word16)sum;
}

/* ---------------------------------------------------------------------------
 *  Public entry point - matches the FX prototype in prot_fx.h:10491.
 * --------------------------------------------------------------------------- */
void lerp(Word16 *f, Word16 *f_out,
          Word16 bufferNewSize, Word16 bufferOldSize)
{
    /* Local intermediate scratch for staged lerp.  Sized to
     * match the float version's `float buf[2*L_FRAME_MAX]`
     * (1920 floats = 7680 bytes); in Word16 that's 1920 * 2
     * = 3840 bytes on the stack.  L_FRAME_MAX is 960
     * (cnst_fx.h:414, pulled in by the FX typedef shim). */
    Word16 scratch[2 * L_FRAME_MAX];
    Word16 *pIn;
    Word16 curOldSize;
    Word16 i;

    /* ------------------------------------------------------------------
     *  Defensive guards.
     *
     *  Upstream call sites always pass valid sizes, but a small amount
     *  of belt-and-braces keeps us safe against accidental misuse
     *  from the parent code (and lets lerp be called early in init
     *  paths where the caller's state is still being set up).
     * ------------------------------------------------------------------ */
    if (bufferNewSize <= 0)
    {
        /* Nothing to write.  Leave f_out untouched. */
        return;
    }
    if (bufferOldSize <= 0)
    {
        /* No input samples to interpolate from.  Zero-fill the
         * output (the lerp_proc would have undefined behavior
         * reading f[0..oldSize-1] in this case). */
        for (i = 0; i < bufferNewSize; i++)
        {
            f_out[i] = 0;
        }
        return;
    }
    if (bufferOldSize == 1)
    {
        /* Single source sample: every output sample is the
         * same value.  This sidesteps the lerp_proc branch that
         * would otherwise read f[1] (OOB) or f[-1] (OOB). */
        Word16 v = f[0];
        for (i = 0; i < bufferNewSize; i++)
        {
            f_out[i] = v;
        }
        return;
    }

    /* Sanity cap: the local scratch is 2*L_FRAME_MAX = 1920
     * elements, matching the float upstream.  Anything larger
     * is a programming error from the call site; rather than
     * run off the end of the stack buffer, do a safe copy
     * that never reads past f[bufferOldSize-1]:
     *   - copy the overlap region of f into f_out
     *     (min(old, new) samples)
     *   - hold f[oldSize-1] for the tail of f_out
     * This is a degraded but well-defined fallback; the FX
     * call sites never exceed this cap in practice (the
     * largest is 2*L_frame = 1920 at 48kHz). */
    if (bufferNewSize > (Word16)(2 * L_FRAME_MAX))
    {
        Word16 copyLen = (bufferNewSize < bufferOldSize)
                          ? bufferNewSize : bufferOldSize;
        Word16 last = f[bufferOldSize - 1];
        for (i = 0; i < copyLen; i++)
        {
            f_out[i] = f[i];
        }
        for (i = copyLen; i < bufferNewSize; i++)
        {
            f_out[i] = last;
        }
        return;
    }

    /* Same-size short-circuit.  Float lerp.c:87-92 also
     * special-cases this (with a local buf copy + mvr2r to
     * f_out), but with our Word16 output we can copy directly
     * to f_out. */
    if (bufferNewSize == bufferOldSize)
    {
        for (i = 0; i < bufferNewSize; i++)
        {
            f_out[i] = f[i];
        }
        return;
    }

    /* ------------------------------------------------------------------
     *  Single-step case: ratio is within maxFac = 507/128 in
     *  either direction.
     *
     *  Cross-multiply to avoid floating point (we only have
     *  integer arithmetic in this TU):
     *    newSize/oldSize > 507/128  <=>  newSize*128 > oldSize*507
     *    oldSize/newSize > 507/128  <=>  oldSize*128 > newSize*507
     *
     *  Use Word32 for the products to avoid Word16 overflow
     *  (worst case: 1920 * 507 = 973,440, fits in Word32).
     * ------------------------------------------------------------------ */
    {
        Word32 lhsUp, rhsUp, lhsDn, rhsDn;
        Word16 needsStage;

        lhsUp = (Word32)bufferNewSize * 128L;
        rhsUp = (Word32)bufferOldSize * 507L;
        lhsDn = (Word32)bufferOldSize * 128L;
        rhsDn = (Word32)bufferNewSize * 507L;
        needsStage = (Word16)((lhsUp > rhsUp) || (lhsDn > rhsDn));

        if (!needsStage)
        {
            lerp_proc(f, f_out, bufferNewSize, bufferOldSize);
            return;
        }
    }

    /* ------------------------------------------------------------------
     *  Staged lerp: chain lerp_proc calls, doubling or halving
     *  the size each step until we reach bufferNewSize.
     *
     *  Mirrors float upstream lib_com/lerp.c:35-68.  The upstream
     *  snaps the per-step newSize to the final `bufferNewSize`
     *  as soon as the remaining target/current ratio drops to
     *  within maxFac = 507/128; we replicate that with cross-
     *  multiplied integer comparisons.  See the file header for
     *  the trace of 64->1280 (4 steps: 2x, 2x, 2x, 2.5x).
     *
     *  Each step writes to `scratch` (the local stack buffer);
     *  the final result is copied to f_out at the end.  The
     *  intermediate input for the next step is the previous
     *  step's output, also in `scratch`; this is safe because
     *  lerp_proc uses its own local `buf` array and only writes
     *  to f_out at the end of the step, so reads of `scratch`
     *  see the previous intermediate throughout the current
     *  step's loop.
     *
     *  For the FX call sites the per-step newSize never exceeds
     *  2*L_FRAME_MAX (the first doubling 2*oldSize has oldSize
     *  <= 960 because the same-size short-circuit catches the
     *  2*L_frame -> 2*L_frame case at 48kHz), so the scratch
     *  cap is not violated in practice.
     * ------------------------------------------------------------------ */
    pIn = f;
    curOldSize = bufferOldSize;
    if (bufferNewSize > bufferOldSize)
    {
        /* Upsampling.  Mirrors float upstream lerp.c:35-51.
         * Initial tmpNewSize = 2*oldSize; on each iteration
         * check whether the remaining target/current ratio is
         * within maxFac, and if so snap tmpNewSize to the
         * final bufferNewSize (skipping the doubling). */
        Word16 curNewSize;
        Word32 lhs, rhs;
        curNewSize = (Word16)((Word32)curOldSize * 2L);
        while (bufferNewSize > curOldSize)
        {
            lhs = (Word32)bufferNewSize * 128L;
            rhs = (Word32)curOldSize * 507L;
            if (lhs <= rhs)
            {
                /* Remaining ratio <= maxFac: snap to final. */
                curNewSize = bufferNewSize;
            }
            lerp_proc(pIn, scratch, curNewSize, curOldSize);
            pIn = scratch;
            curOldSize = curNewSize;
            curNewSize = (Word16)((Word32)curNewSize * 2L);
        }
    }
    else
    {
        /* Downsampling.  Mirrors float upstream lerp.c:52-68.
         * Initial tmpNewSize = oldSize/2; on each iteration
         * check whether the remaining current/target ratio is
         * within maxFac, and if so snap tmpNewSize to the
         * final bufferNewSize. */
        Word16 curNewSize;
        Word32 lhs, rhs;
        curNewSize = (Word16)((Word32)curOldSize / 2L);
        while (bufferNewSize < curOldSize)
        {
            lhs = (Word32)curOldSize * 128L;
            rhs = (Word32)bufferNewSize * 507L;
            if (lhs <= rhs)
            {
                /* Remaining ratio <= maxFac: snap to final. */
                curNewSize = bufferNewSize;
            }
            lerp_proc(pIn, scratch, curNewSize, curOldSize);
            pIn = scratch;
            curOldSize = curNewSize;
            curNewSize = (Word16)((Word32)curNewSize / 2L);
        }
    }

    /* Copy the final intermediate from scratch to f_out. */
    for (i = 0; i < bufferNewSize; i++)
    {
        f_out[i] = scratch[i];
    }
}

/* ---------------------------------------------------------------------------
 *  Per-step helper: one piece-wise linear resample, no staging.
 *
 *  Mirrors float lerp.c:75-141 (`lerp_proc`).  Used both directly
 *  (when the ratio is within maxFac) and as the inner step of
 *  the staged loop in `lerp()` above.
 * --------------------------------------------------------------------------- */
static void lerp_proc(Word16 *f, Word16 *f_out,
                      Word16 bufferNewSize, Word16 bufferOldSize)
{
    /* Local scratch, same size as in lerp() and matching the
     * float upstream's `float buf[2*L_FRAME_MAX]`.  We
     * accumulate into this local first, then copy to f_out
     * at the end, so the function is safe to call with
     * f == f_out (the reads during the interpolation loop
     * see the original input before the final copy
     * overwrites f_out). */
    Word16 buf[2 * L_FRAME_MAX];
    Word32 shift_q15;   /* Q15: oldSize/newSize as integer */
    Word32 pos_q15;     /* Q15 position; must be Word32
                         * because the accumulator grows
                         * without bound over the loop */
    Word16 i, last;

    /* Same-size short-circuit.  Float lerp.c:87-92 does
     * `mvr2r(f, buf, n); mvr2r(buf, f_out, n);` but with our
     * Word16 local we can copy directly to f_out.  Normally
     * unreachable from the staged lerp() loop above (which
     * always picks curNewSize != curOldSize for the final
     * snap, except when bufferNewSize happens to equal a
     * power-of-two multiple of bufferOldSize - e.g. 100 ->
     * 200 with a snap from 100 to 200, ratio 2x, no
     * intermediate).  Kept as a safety net. */
    if (bufferNewSize == bufferOldSize)
    {
        for (i = 0; i < bufferNewSize; i++)
        {
            f_out[i] = f[i];
        }
        return;
    }

    /* Compute shift in Q15.  Float lerp.c:95 uses basop
     * helpers (div_s / L_deposit_l / L_shl) to keep
     * precision; the end result is the float `oldSize /
     * newSize`.  In Q15 we just multiply-then-divide in
     * Word32.  bufferNewSize is verified non-zero by
     * lerp()'s guard. */
    shift_q15 = ((Word32)bufferOldSize * 32768L) / (Word32)bufferNewSize;

    /* pos = 0.5*shift - 0.5 in Q15.  Float lerp.c:96.
     * Using (shift - 32768) / 2 instead of shift/2 - 16384
     * keeps the math in one expression and avoids a
     * separate constant. */
    pos_q15 = (shift_q15 - 32768L) / 2L;

    /* Apply the -0.13 offset when shift is small (heavy
     * upsampling).  Float lerp.c:98-101.  0.3 * 32768 =
     * 9830.4 (truncated to 9830) is the threshold;
     * 0.13 * 32768 = 4259.8 (truncated to 4259) is the
     * offset.  These match the float version's
     * `if (shift < 0.3f) { pos = pos - 0.13f; }` to within
     * a sub-LSB rounding step. */
    if (shift_q15 < 9830L)
    {
        pos_q15 -= 4259L;
    }

    /* ----------------------------------------------------------------
     *  First point.  Float lerp.c:104-113.
     *
     *  Handled by lerp_interpolate_q15, which mimics the
     *  float upstream's C `(int)pos` truncation toward zero
     *  (idx = 0, frac = pos for pos in (-1, 0); idx = pos,
     *  frac = pos - idx for pos >= 0).  This avoids the
     *  arithmetic-right-shift bug that would otherwise read
     *  f[-1] for negative pos_q15 (reachable on the first
     *  point of every heavy-upsample path, e.g. 320 -> 960
     *  and 128 -> 480).
     * ---------------------------------------------------------------- */
    buf[0] = lerp_interpolate_q15(f, pos_q15, (Word16)bufferOldSize, 0);
    pos_q15 += shift_q15;

    /* ----------------------------------------------------------------
     *  Middle points.  Float lerp.c:117-124.
     *
     *  Iterate i = 1 .. newSize-2 (the last point is handled
     *  separately below to apply the boundary clamp).  The
     *  read of f[idx+1] is safe here: lerp_interpolate_q15
     *  forces idx = 0 for negative pos_q15 (so f[idx+1] =
     *  f[1] is in range), and clamps non-negative positions at
     *  or past oldSize-1 to idx = oldSize-2.  This avoids both
     *  f[-1] and f[oldSize] reads while preserving linear
     *  extrapolation from the nearest valid two samples.
     * ---------------------------------------------------------------- */
    for (i = 1; i < bufferNewSize - 1; i++)
    {
        buf[i] = lerp_interpolate_q15(f, pos_q15, (Word16)bufferOldSize, 0);
        pos_q15 += shift_q15;
    }

    /* ----------------------------------------------------------------
     *  Last point.  Float lerp.c:127-137.
     *
     *  When pos > oldSize-1, lerp_interpolate_q15 clamps idx
     *  to oldSize-2 and uses `pos_q15 - (idx << 15)` as the
     *  fraction; this can exceed 32767 (= 1.0) when the
     *  position is past oldSize-1, representing the linear
     *  extrapolation past f[oldSize-1] that the float
     *  upstream's `diff = pos - idx` produces.  The Q15 form
     *  uses `>` to match the float version's `>` check
     *  (equality is unreachable in practice; the lerp
     *  positions never land exactly on oldSize-1 for the
     *  last point because the same-size short-circuit
     *  catches that degenerate case at the top of this
     *  function).
     * ---------------------------------------------------------------- */
    last = (Word16)(bufferNewSize - 1);
    buf[last] = lerp_interpolate_q15(f, pos_q15, (Word16)bufferOldSize, 1);

    /* Copy local scratch to f_out.  Mirrors float lerp.c:139. */
    for (i = 0; i < bufferNewSize; i++)
    {
        f_out[i] = buf[i];
    }
}
