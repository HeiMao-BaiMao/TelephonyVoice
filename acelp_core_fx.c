/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Provides fourteen ACELP core encoder symbols that are declared in
 *      external/3gpp-evs/lib_com/prot_fx.h
 *  but have no usable fixed-point implementation in the vendored 3GPP EVS
 *  snapshot:
 *
 *      E_ACELP_xy2_corr
 *      E_ACELP_adaptive_codebook
 *      E_ACELP_innovative_codebook
 *      E_ACELP_weighted_code
 *      E_ACELP_4tsearch
 *      E_ACELP_4tsearchx
 *      E_ACELP_indexing
 *      E_ACELP_hh_corr
 *      E_ACELP_toeplitz_mul
 *      E_ACELP_conv
 *      E_GAIN_closed_loop_search
 *      encode_acelp_gains
 *      BITS_ALLOC_config_acelp
 *      Unified_weighting_fx
 *
 *  Why this parent-side helper exists
 *  ----------------------------------
 *  The upstream float implementations (where they exist) live under
 *  external/3gpp-evs/lib_enc/ and external/3gpp-evs/lib_com/ and have
 *  float/int ABIs that cannot be linked into the FX static libraries.
 *  The vendored fixed-point snapshot declares the FX prototypes in
 *  prot_fx.h but does not ship the corresponding FX implementations,
 *  so unresolved-symbol errors block the FX link.  Because
 *  external/3gpp-evs/ is read-only, the helpers are provided in the
 *  parent repo and are intended to be attached to evs-lib-com-fx via
 *  EVS_FX_EXTRAS_LIB_COM in cmake/3gpp-evs.cmake.
 *
 *  Numerical contract
 *  ------------------
 *  The implementations below are primarily "link-unblock" approximations.
 *  Functions that map cleanly to a small fixed-point algorithm
 *  (correlation, convolution, Toeplitz multiply, bit allocation) are
 *  implemented in a straightforward way.  The larger algebraic
 *  codebook search, adaptive/innovative codebook construction, gain
 *  quantisation, and LSF weighting are either partially implemented or
 *  stubbed with sensible defaults; each stub is marked with a TODO.
 *  Bit-exact fidelity to a missing upstream FX reference is a later
 *  refinement goal.
 *
 *  Implementation notes
 *  --------------------
 *  Only "typedefs.h" and "prot_fx.h" are included.  The FX typedef shim
 *  force-includes the Word16/Word32/basic-op environment, so the same FX
 *  include wiring is used as the rest of evs-lib-com-fx.
 * ============================================================================ */

#include "typedefs.h"
#include "prot_fx.h"

/* ============================================================================
 * Local saturation / normalization helpers (plain C, no extra basop deps)
 * ============================================================================ */

static Word16 saturate16(Word32 x)
{
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (Word16)x;
}

static Word32 saturate32(long long x)
{
    if (x > 2147483647LL) return 2147483647L;
    if (x < -2147483648LL) return -2147483648L;
    return (Word32)x;
}

/* Return the number of leading sign bits of a 32-bit value (0..31). */
static Word16 norm32(Word32 x)
{
    Word16 n = 0;
    Word32 y;
    if (x == 0) return 31;
    y = (x < 0) ? ~x : x;
    while ((y & 0x40000000L) == 0)
    {
        n++;
        y <<= 1;
    }
    return n;
}

/* ============================================================================
 * E_ACELP_xy2_corr
 *
 * Compute the six correlations needed by the ACELP gain quantiser and store
 * them in g_corr with a common exponent.  The upstream float reference would
 * keep these as floats; the FX convention stores a mantissa and an exponent.
 * Call sites (e.g. enc_gen_voic_rf_fx.c) adjust the exponents afterwards to
 * match their own Q-domain assumptions, so a reasonable choice here is to
 * return the saturated mantissa with the exponent required to reconstruct the
 * true energy in Q(2*exp_xn) / Q(exp_xn+9) etc.
 * ============================================================================ */
void E_ACELP_xy2_corr(
    Word16 xn[],
    Word16 y1[],
    Word16 y2[],
    ACELP_CbkCorr *g_corr,
    Word16 L_subfr,
    Word16 exp_xn)
{
    Word16 i;
    long long xx = 0, y1y1 = 0, y2y2 = 0, xy1 = 0, xy2 = 0, y1y2 = 0;
    Word32 max_abs, shift;

    for (i = 0; i < L_subfr; i++)
    {
        xx   += (long long)xn[i] * xn[i];
        y1y1 += (long long)y1[i] * y1[i];
        y2y2 += (long long)y2[i] * y2[i];
        xy1  += (long long)xn[i] * y1[i];
        xy2  += (long long)xn[i] * y2[i];
        y1y2 += (long long)y1[i] * y2[i];
    }

    /* Normalize all six to a common 16-bit mantissa with exponent 0.
     * The caller will apply the per-term Q corrections afterwards. */
    max_abs = (Word32)xx;
    if ((Word32)y1y1 > max_abs) max_abs = (Word32)y1y1;
    if ((Word32)y2y2 > max_abs) max_abs = (Word32)y2y2;
    if ((Word32)xy1  > max_abs) max_abs = (Word32)xy1;
    if ((Word32)xy2  > max_abs) max_abs = (Word32)xy2;
    if ((Word32)y1y2 > max_abs) max_abs = (Word32)y1y2;

    if (max_abs == 0)
    {
        g_corr->xx = g_corr->y1y1 = g_corr->y2y2 = 0;
        g_corr->xy1 = g_corr->xy2 = g_corr->y1y2 = 0;
        g_corr->xx_e = g_corr->y1y1_e = g_corr->y2y2_e = 0;
        g_corr->xy1_e = g_corr->xy2_e = g_corr->y1y2_e = 0;
        return;
    }

    /* Shift left so max_abs is at least 2^14, giving a usable mantissa. */
    shift = norm32(max_abs) - 17;
    if (shift < 0) shift = 0;

    g_corr->xx   = saturate16((Word32)(xx   << shift));
    g_corr->y1y1 = saturate16((Word32)(y1y1 << shift));
    g_corr->y2y2 = saturate16((Word32)(y2y2 << shift));
    g_corr->xy1  = saturate16((Word32)(xy1  << shift));
    g_corr->xy2  = saturate16((Word32)(xy2  << shift));
    g_corr->y1y2 = saturate16((Word32)(y1y2 << shift));

    g_corr->xx_e   = (Word16)(-shift);
    g_corr->y1y1_e = (Word16)(-shift);
    g_corr->y2y2_e = (Word16)(-shift);
    g_corr->xy1_e  = (Word16)(-shift);
    g_corr->xy2_e  = (Word16)(-shift);
    g_corr->y1y2_e = (Word16)(-shift);

    (void)exp_xn;
}

/* ============================================================================
 * E_ACELP_adaptive_codebook
 *
 * Generate the adaptive-codebook contribution y1, update the excitation,
 * estimate the closed-loop pitch gain, and append quantiser indices.
 * TODO: This is a link-unblock stub.  It zero-fills the outputs so the
 * encoder continues to link and run; a proper port should call pred_lt4_fx
 * with the fractional pitch lag, filter the result through h1, and run the
 * gain/quantisation path.
 * ============================================================================ */
void E_ACELP_adaptive_codebook(
    Word16 *exc,
    Word16 T0,
    Word16 T0_frac,
    Word16 T0_res,
    Word16 T0_res_max,
    Word16 mode,
    Word16 i_subfr,
    Word16 L_subfr,
    Word16 L_frame,
    Word16 *h1,
    Word16 clip_gain,
    Word16 *xn,
    Word16 *y1,
    ACELP_CbkCorr *g_corr,
    Word16 **pt_indice,
    Word16 *pitch_gain,
    Word16 exp_xn,
    Word16 rf_mode,
    Word16 use_prev_sf_pit_gain,
    Word16* lp_select)
{
    Word16 i;

    for (i = 0; i < L_subfr; i++)
    {
        y1[i] = 0;
        exc[i_subfr + i] = 0;
    }

    if (g_corr != NULL)
    {
        g_corr->xx = g_corr->y1y1 = g_corr->y2y2 = 0;
        g_corr->xy1 = g_corr->xy2 = g_corr->y1y2 = 0;
        g_corr->xx_e = g_corr->y1y1_e = g_corr->y2y2_e = 0;
        g_corr->xy1_e = g_corr->xy2_e = g_corr->y1y2_e = 0;
    }

    *pitch_gain = 0;
    if (lp_select != NULL) *lp_select = 0;

    (void)T0;
    (void)T0_frac;
    (void)T0_res;
    (void)T0_res_max;
    (void)mode;
    (void)L_frame;
    (void)h1;
    (void)clip_gain;
    (void)xn;
    (void)exp_xn;
    (void)rf_mode;
    (void)use_prev_sf_pit_gain;
    (void)pt_indice;
}

/* ============================================================================
 * E_ACELP_innovative_codebook
 *
 * Drive the fixed (algebraic) codebook search and produce the innovation
 * code[], the filtered innovation y2[], and any associated indices.
 * TODO: This is a link-unblock stub.  It zero-fills code[] and y2[]; a proper
 * implementation should update the target, shape h1, call E_ACELP_4tsearch/
 * E_ACELP_4tsearchx, and apply pitch/formant shaping.
 * ============================================================================ */
void E_ACELP_innovative_codebook(
    Word16 *exc,
    Word16 T0,
    Word16 T0_frac,
    Word16 T0_res,
    Word16 gain_pit,
    Word16 tilt_code,
    Word16 mode,
    Word16 formant_enh,
    Word16 formant_tilt,
    const Word16 formant_enh_num,
    const Word16 foramnt_enh_den,
    Word16 pitch_sharpening,
    Word16 pre_emphasis,
    Word16 phase_scrambling,
    Word16 i_subfr,
    const Word16 *Aq,
    Word16 *h1,
    Word16 *xn,
    Word16 *cn,
    Word16 *y1,
    Word16 *y2,
    Word8 acelpautoc,
    Word16 **pt_indice,
    Word16 *code,
    Word16 shift,
    const Word16 L_frame,
    const Word16 last_L_frame,
    const Word32 total_brate)
{
    Word16 i;
    for (i = 0; i < L_SUBFR; i++)
    {
        code[i] = 16;
        y2[i] = 0;
    }

    (void)exc;
    (void)T0;
    (void)T0_frac;
    (void)T0_res;
    (void)gain_pit;
    (void)tilt_code;
    (void)mode;
    (void)formant_enh;
    (void)formant_tilt;
    (void)formant_enh_num;
    (void)foramnt_enh_den;
    (void)pitch_sharpening;
    (void)pre_emphasis;
    (void)phase_scrambling;
    (void)i_subfr;
    (void)Aq;
    (void)h1;
    (void)xn;
    (void)cn;
    (void)y1;
    (void)acelpautoc;
    (void)pt_indice;
    (void)shift;
    (void)L_frame;
    (void)last_L_frame;
    (void)total_brate;
}

/* ============================================================================
 * E_ACELP_weighted_code
 *
 * Convolve the sparse algebraic codevector with the weighted synthesis
 * impulse response: y = H * code.  code is Q9, H is Q, and the expected
 * output is Q12 (cod4t64_fx.c post-shifts by 3 to obtain Q9).
 * ============================================================================ */
void E_ACELP_weighted_code(
    const Word16 code[],
    const Word16 H[],
    Word16 Q,
    Word16 y[])
{
    Word16 i, j;
    Word16 shift;
    long long s;

    /* Shift from code(Q9)*H(Q) = Q(9+Q) down to Q12. */
    shift = (Word16)(Q - 3);
    if (shift < 0) shift = 0;

    for (i = 0; i < L_SUBFR; i++)
    {
        s = 0;
        for (j = 0; j <= i; j++)
        {
            s += (long long)code[j] * H[i - j];
        }
        if (shift > 0)
        {
            s = (s + (1LL << (shift - 1))) >> shift;
        }
        y[i] = saturate16((Word32)s);
    }
}

/* ============================================================================
 * Float ACELP helper entry points (compiled into evs-float-acelp)
 * ============================================================================ */
extern void evs_fx_E_ACELP_4tsearch(
    Float32 dn[], const Float32 cn[], const Float32 H[], float code[],
    PulseConfig *config, Word16 ind[], Float32 y[]);

extern void evs_fx_E_ACELP_4tsearchx(
    Float32 dn[], const Float32 cn[], Float32 Rw[], float code[],
    PulseConfig *config, Word16 ind[]);

extern short evs_fx_E_ACELP_indexing(
    Float32 code[], PulseConfig config, int num_tracks, int prm[]);

/* Scale a fixed-point sample to a normalised float for the algebraic search.
 * The exact Q-domain is discarded; only the relative shape matters to the
 * pulse-position selection. */
static float fx_to_float(Word16 x)
{
    return (float)x;
}

/* Convert the float algebraic codevector (pulses of +/-1.0) to Q9. */
static Word16 float_to_q9(float x)
{
    float v = x * 512.0f;
    if (v >= 0.0f) v += 0.5f;
    else           v -= 0.5f;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (Word16)v;
}

/* ----------------------------------------------------------------------------
 * PulseConfig translation from the FX namespace to the float helper namespace.
 *
 * The FX cnst_fx.h and the float cnst.h share the same TRACKPOS semantics but
 * disagree on the numeric value of TRACKPOS_FREE_THREE (5 vs 6).  The float
 * ACELP helper was compiled against the float enum, so we must translate the
 * codetrackpos field before passing an FX PulseConfig to it.
 *
 * The FX code stores config.alp as a Q13 fixed-point integer (e.g. 6144 means
 * 0.75) even though the shared PulseConfig.alp field is typed as float.  The
 * float search expects a normalised floating-point energy, so convert it back.
 * --------------------------------------------------------------------------- */
static int codetrackpos_fx_to_float(int fx)
{
    switch (fx)
    {
    case 0:  return 0;  /* TRACKPOS_FIXED_FIRST        */
    case 1:  return 1;  /* TRACKPOS_FIXED_EVEN         */
    case 2:  return 2;  /* TRACKPOS_FIXED_FIRST_TWO    */
    case 3:  return 3;  /* TRACKPOS_FIXED_TWO          */
    case 4:  return 4;  /* TRACKPOS_FREE_ONE           */
    case 5:  return 6;  /* TRACKPOS_FREE_THREE (FX=5, float=6) */
    default: return fx;
    }
}

static PulseConfig pulseconfig_fx_to_float(const PulseConfig *fx)
{
    PulseConfig flt = *fx;
    flt.codetrackpos = (enum TRACKPOS)codetrackpos_fx_to_float((int)fx->codetrackpos);
    flt.alp = fx->alp / 8192.0f;
    return flt;
}

/* ============================================================================
 * E_ACELP_4tsearch
 *
 * Deep-first algebraic codebook search (covariance method).
 * Runs the upstream float search on FX inputs, converts the resulting float
 * codevector back to Q9, and recomputes the filtered codevector y in the FX
 * Q-domain.
 * ============================================================================ */
void E_ACELP_4tsearch(
    Word16 dn[],
    const Word16 cn[],
    const Word16 H[],
    Word16 code[],
    const PulseConfig *config,
    Word16 ind[],
    Word16 y[])
{
    Float32 dn_f[L_SUBFR];
    Float32 cn_f[L_SUBFR];
    Float32 H_f[L_SUBFR];
    Float32 y_f[L_SUBFR];
    float code_f[L_SUBFR];
    Word16 i;

    for (i = 0; i < L_SUBFR; i++)
    {
        dn_f[i] = fx_to_float(dn[i]);
        cn_f[i] = fx_to_float(cn[i]);
        H_f[i]  = fx_to_float(H[i]);
    }

    {
        PulseConfig cfg = pulseconfig_fx_to_float(config);
        evs_fx_E_ACELP_4tsearch(dn_f, cn_f, H_f, code_f, &cfg, ind, y_f);
    }

    for (i = 0; i < L_SUBFR; i++)
    {
        code[i] = float_to_q9(code_f[i]);
    }

    /* Recompute y in the FX Q-domain so downstream gain quantisation sees the
     * same scaling as the original cod4t64_fx.c contract (y is Q12 here; the
     * caller shifts to Q9). */
    E_ACELP_weighted_code(code, H, 12, y);
}

/* ============================================================================
 * E_ACELP_4tsearchx
 *
 * Deep-first algebraic codebook search (autocorrelation method).
 * Runs the upstream float search and returns a Q9 codevector; the caller is
 * responsible for filtering it through E_ACELP_weighted_code.
 * ============================================================================ */
void E_ACELP_4tsearchx(
    Word16 dn[],
    const Word16 cn[],
    Word16 Rw[],
    Word16 code[],
    const PulseConfig *config,
    Word16 ind[])
{
    Float32 dn_f[L_SUBFR];
    Float32 cn_f[L_SUBFR];
    Float32 Rw_f[L_SUBFR];
    float code_f[L_SUBFR];
    Word16 i;

    for (i = 0; i < L_SUBFR; i++)
    {
        dn_f[i] = fx_to_float(dn[i]);
        cn_f[i] = fx_to_float(cn[i]);
        Rw_f[i] = fx_to_float(Rw[i]);
    }

    {
        PulseConfig cfg = pulseconfig_fx_to_float(config);
        evs_fx_E_ACELP_4tsearchx(dn_f, cn_f, Rw_f, code_f, &cfg, ind);
    }

    for (i = 0; i < L_SUBFR; i++)
    {
        code[i] = float_to_q9(code_f[i]);
    }
}

/* ============================================================================
 * E_ACELP_indexing
 *
 * Pack the algebraic codevector into bit-stream indices using the upstream
 * float indexing.  Indices are returned as Word16 to match the FX contract.
 * ============================================================================ */
Word16 E_ACELP_indexing(
    const Word16 code[],
    const PulseConfig *config,
    Word16 num_tracks,
    Word16 prm[])
{
    Float32 code_f[L_SUBFR];
    int prm_i[8];
    Word16 i;
    Word16 wordcnt = (Word16)((config->bits + 15) >> 4);
    short saved;
    PulseConfig cfg = pulseconfig_fx_to_float(config);

    for (i = 0; i < L_SUBFR; i++)
    {
        code_f[i] = (Float32)code[i] / 512.0f;
    }

    saved = evs_fx_E_ACELP_indexing(code_f, cfg, num_tracks, prm_i);

    for (i = 0; i < wordcnt; i++)
    {
        prm[i] = (Word16)prm_i[i];
    }

    return saved;
}

/* ============================================================================
 * E_ACELP_hh_corr
 *
 * Compute the symmetric autocorrelation R[k] = sum_i h[i]*h[i+k] used by the
 * autocorrelation method of innovation search.  Return the Q-domain exponent
 * of the output R[].
 * ============================================================================ */
Word16 E_ACELP_hh_corr(
    Word16 *x,
    Word16 *y,
    Word16 L_subfr,
    Word16 bits)
{
    Word16 i, k;
    long long acc;
    long long max_abs = 0;
    Word16 shift;
    Word16 out[L_SUBFR];

    for (k = 0; k < L_subfr; k++)
    {
        acc = 0;
        for (i = 0; i < L_subfr - k; i++)
        {
            acc += (long long)x[i] * x[i + k];
        }
        if (acc < 0) acc = -acc;
        if (acc > max_abs) max_abs = acc;
        out[k] = 0;
    }

    /* Normalize into 16-bit signed range. */
    if (max_abs == 0)
    {
        for (k = 0; k < L_subfr; k++) y[k] = 0;
        return 0;
    }

    shift = norm32((Word32)max_abs);
    if (shift > 16) shift = 16;

    for (k = 0; k < L_subfr; k++)
    {
        acc = 0;
        for (i = 0; i < L_subfr - k; i++)
        {
            acc += (long long)x[i] * x[i + k];
        }
        acc <<= shift;
        y[k] = saturate16((Word32)acc);
    }

    (void)bits;
    return (Word16)(2 * (11 + 1) - shift); /* h is ~Q11, product Q22, minus normalisation */
}

/* ============================================================================
 * E_ACELP_toeplitz_mul
 *
 * Multiply a symmetric Toeplitz matrix R by vector c: d = Toeplitz(R) * c.
 * The float reference is acelp_enc_util.c.
 * ============================================================================ */
Word16 E_ACELP_toeplitz_mul(
    const Word16 R[],
    const Word16 c[],
    Word16 d[],
    const Word16 L_subfr,
    const Word16 highrate)
{
    Word16 k, j;
    long long s;

    for (k = 0; k < L_subfr; k++)
    {
        s = (long long)R[k] * c[0];

        for (j = 1; j < k; j++)
        {
            s += (long long)R[k - j] * c[j];
        }

        for (; j < L_subfr; j++)
        {
            s += (long long)R[j - k] * c[j];
        }

        d[k] = saturate16((Word32)s);
    }

    (void)highrate;
    return 0;
}

/* ============================================================================
 * E_ACELP_conv
 *
 * Inverse filtering: convert target xn2 into residual-domain target cn2 using
 * the shaped impulse response h2.  Float reference is enc_acelp.c:
 *      cn2[k] = xn2[k] - sum_{i=0}^{k-1} cn2[i] * h2[k-i]
 * ============================================================================ */
void E_ACELP_conv(
    const Word16 xn2[],
    const Word16 h2[],
    Word16 cn2[])
{
    Word16 k, i;
    long long sum;

    for (k = 0; k < L_SUBFR; k++)
    {
        sum = (long long)xn2[k] << 12; /* bring xn2 into the h2(Q12) product domain */
        for (i = 0; i < k; i++)
        {
            sum -= (long long)cn2[i] * h2[k - i];
        }
        cn2[k] = saturate16((Word32)((sum + (1LL << 11)) >> 12));
    }
}

/* ============================================================================
 * E_GAIN_closed_loop_search
 *
 * Closed-loop fractional pitch search.  This is a simplified FX port of the
 * float reference in enc_gain.c, using the existing FX helpers norm_corr_fx()
 * and Interpol_4() from the vendored tree.
 * ============================================================================ */
Word16 E_GAIN_closed_loop_search(
    Word16 exc[],
    Word16 xn[], Word16 h[],
    Word16 t0_min, Word16 t0_min_frac, Word16 t0_max, Word16 t0_max_frac,
    Word16 t0_min_max_res, Word16 *pit_frac, Word16 *pit_res, Word16 pit_res_max,
    Word16 i_subfr, Word16 pit_min, Word16 pit_fr2, Word16 pit_fr1, Word16 L_subfr)
{
    Word16 t_min, t_max;
    Word16 corr_v[15 + 2 * L_INTERPOL1 + 1];
    Word16 *corr;
    Word16 max, t0, t1;
    Word16 fraction, step, temp, cor_max;

    (void)t0_min_frac;
    (void)t0_max_frac;
    (void)t0_min_max_res;
    (void)pit_min;

    t_min = sub(t0_min, L_INTERPOL1);
    t_max = add(t0_max, L_INTERPOL1);
    corr = &corr_v[-t_min];

    norm_corr_fx(exc, xn, h, t_min, t_max, corr, L_subfr);

    /* integer pitch */
    max = corr[t0_min];
    t0 = t0_min;
    for (t1 = add(t0_min, 1); t1 <= t0_max; t1++)
    {
        if (corr[t1] >= max)
        {
            max = corr[t1];
            t0 = t1;
        }
    }

    /* first subframe, no fractional search if t0 >= pit_fr1 */
    if ((i_subfr == 0) && (sub(t0, pit_fr1) >= 0))
    {
        *pit_frac = 0;
        *pit_res = 1;
        return t0;
    }

    /* fractional search around t0 */
    t1 = t0;
    step = 1;
    fraction = 1;

    if (((i_subfr == 0) && (sub(t0, pit_fr2) >= 0)) || (sub(pit_fr2, pit_min) <= 0))
    {
        step = 2;
        fraction = 2;
    }

    if (sub(t0, t0_min) == 0)
    {
        fraction = 0;
        cor_max = Interpol_4(&corr[t0], fraction);
    }
    else
    {
        t0 = sub(t0, 1);
        cor_max = Interpol_4(&corr[t0], fraction);
        for (temp = add(fraction, step); temp <= 3; temp = add(temp, step))
        {
            Word16 v = Interpol_4(&corr[t0], temp);
            if (sub(v, cor_max) > 0)
            {
                cor_max = v;
                fraction = temp;
            }
        }
    }

    for (temp = 0; temp <= 3; temp = add(temp, step))
    {
        Word16 v = Interpol_4(&corr[t1], temp);
        if (sub(v, cor_max) > 0)
        {
            cor_max = v;
            fraction = temp;
            t0 = t1;
        }
    }

    *pit_frac = fraction;
    *pit_res = pit_res_max;

    if (((i_subfr == 0) && (sub(t0, pit_fr2) >= 0)) || (sub(pit_fr2, pit_min) <= 0))
    {
        *pit_res = shr(pit_res_max, 1);
        *pit_frac = shr(fraction, 1);
    }

    return t0;
}

/* ============================================================================
 * encode_acelp_gains
 *
 * Quantise adaptive and innovative codebook gains.
 * TODO: This is a link-unblock stub.  It sets safe default gains and appends
 * one zero index.  A proper port would implement the 5/6/7-bit VQ tables and
 * UV/GACELP_UV paths from q_gain2p.c.
 * ============================================================================ */
void encode_acelp_gains(
    Word16 *code,
    Word16 gains_mode,
    Word16 mean_ener_code,
    Word16 clip_gain,
    ACELP_CbkCorr *g_corr,
    Word16 *gain_pit,
    Word32 *gain_code,
    Word16 **pt_indice,
    Word32 *past_gcode,
    Word16 *gain_inov,
    Word16 L_subfr,
    Word16 *code2,
    Word32 *gain_code2,
    Word8 noisy_speech_flag)
{
    Word16 i;
    long long ener = 0;

    for (i = 0; i < L_subfr; i++)
    {
        ener += (long long)code[i] * code[i];
    }
    if (ener <= 0) ener = 1;

    /* gain_inov ~ 1 / sqrt(ener/L_subfr) in Q12 */
    *gain_inov = 4096;
    *gain_pit = 0;
    *gain_code = (Word32)(mean_ener_code << 8);
    if (*gain_code < 1) *gain_code = 1;
    *past_gcode = *gain_code;

    if (gain_code2 != NULL) *gain_code2 = 0;

    **pt_indice = 0;
    (*pt_indice)++;

    (void)clip_gain;
    (void)g_corr;
    (void)code2;
    (void)noisy_speech_flag;
}

/* ============================================================================
 * BITS_ALLOC_config_acelp
 *
 * Configure ACELP bit allocation.  This is a direct fixed-point port of the
 * float reference in lib_com/bits_alloc.c, using the FX ROM tables declared
 * in rom_com_fx.h.
 * ============================================================================ */
Word16 BITS_ALLOC_config_acelp(
    const Word16 bits_frame,
    const Word16 coder_type,
    ACELP_config *acelp_cfg,
    const Word16 narrowband,
    const Word16 nb_subfr)
{
    Word16 mode_index;
    Word16 band_index;
    Word16 i;
    Word16 remaining_bits, bits;
    Word16 bitsused, bits_currsubframe;
    Word16 k;

    mode_index = acelp_cfg->mode_index;
    band_index = (narrowband == 0) ? 1 : 0;
    bits = 0;

    if (band_index == 0)
    {
        if (coder_type == INACTIVE)
        {
            acelp_cfg->formant_enh = 0;
        }
        else
        {
            acelp_cfg->formant_enh = 1;
        }
    }

    if (band_index == 1 && nb_subfr == NB_SUBFR)
    {
        if (coder_type == INACTIVE)
        {
            acelp_cfg->pre_emphasis = 0;
            acelp_cfg->formant_enh = 0;
            acelp_cfg->formant_tilt = 1;
            acelp_cfg->voice_tilt = 1;
        }
        else
        {
            acelp_cfg->pre_emphasis = 1;
            acelp_cfg->formant_enh = 1;
            acelp_cfg->formant_tilt = 0;
            acelp_cfg->voice_tilt = 0;
        }
    }

    if (coder_type == UNVOICED)
    {
        if (ACELP_GAINS_MODE[mode_index][band_index][coder_type] == 6)
        {
            acelp_cfg->pitch_sharpening = 0;
            acelp_cfg->phase_scrambling = 1;
        }
        else
        {
            acelp_cfg->pitch_sharpening = 0;
            acelp_cfg->phase_scrambling = 0;
        }
    }
    else
    {
        acelp_cfg->pitch_sharpening = 1;
        acelp_cfg->phase_scrambling = 0;
    }

    if (coder_type > ACELP_MODE_MAX)
    {
        acelp_cfg->pitch_sharpening = 0;
        acelp_cfg->phase_scrambling = 0;
    }

    acelp_cfg->bpf_mode = ACELP_BPF_MODE[mode_index][band_index][coder_type];
    bits += ACELP_BPF_BITS[acelp_cfg->bpf_mode];

    acelp_cfg->nrg_mode = ACELP_NRG_MODE[mode_index][band_index][coder_type];
    acelp_cfg->nrg_bits = ACELP_NRG_BITS[acelp_cfg->nrg_mode];
    bits += acelp_cfg->nrg_bits;

    acelp_cfg->ltp_mode = ACELP_LTP_MODE[mode_index][band_index][coder_type];
    acelp_cfg->ltp_bits = 0;
    acelp_cfg->ltf_mode = ACELP_LTF_MODE[mode_index][band_index][coder_type];
    acelp_cfg->ltf_bits = ACELP_LTF_BITS[acelp_cfg->ltf_mode];

    if (nb_subfr == NB_SUBFR16k && acelp_cfg->ltf_bits == 4)
    {
        acelp_cfg->ltf_bits++;
    }
    bits += acelp_cfg->ltf_bits;

    for (i = 0; i < nb_subfr; i++)
    {
        acelp_cfg->gains_mode[i] = ACELP_GAINS_MODE[mode_index][band_index][coder_type];

        if (coder_type >= ACELP_MODE_MAX && (i == 1 || i == 3))
        {
            acelp_cfg->gains_mode[i] = 0;
        }

        bits += ACELP_GAINS_BITS[acelp_cfg->gains_mode[i]];
        bits += ACELP_LTP_BITS_SFR[acelp_cfg->ltp_mode][i];
        acelp_cfg->ltp_bits += ACELP_LTP_BITS_SFR[acelp_cfg->ltp_mode][i];
    }

    /* Innovation */
    if (bits_frame < bits)
    {
        return -1;
    }

    if (coder_type == RF_ALLPRED)
    {
        for (i = 0; i < nb_subfr; i++)
        {
            acelp_cfg->fixed_cdk_index[i] = -1;
        }
    }
    else if (coder_type == RF_GENPRED)
    {
        acelp_cfg->fixed_cdk_index[0] = 0;
        acelp_cfg->fixed_cdk_index[1] = -1;
        acelp_cfg->fixed_cdk_index[2] = 0;
        acelp_cfg->fixed_cdk_index[3] = -1;
        acelp_cfg->fixed_cdk_index[4] = -1;
        bits += 14;
    }
    else if (coder_type == RF_NOPRED)
    {
        for (i = 0; i < nb_subfr; i++)
        {
            acelp_cfg->fixed_cdk_index[i] = 0;
        }
        bits += 28;
    }
    else
    {
        Word16 budget;
        budget = (Word16)(bits_frame - bits);

        if (budget < (Word16)(ACELP_FIXED_CDK_BITS(0) * nb_subfr))
        {
            return (Word16)(bits + 1); /* alarm */
        }

        /* find starting cdk index */
        k = 0;
        for (; k < ACELP_FIXED_CDK_NB - 1; k++)
        {
            if ((Word16)(ACELP_FIXED_CDK_BITS(k) * nb_subfr) > budget)
            {
                k--;
                break;
            }
        }
        if ((Word16)(ACELP_FIXED_CDK_BITS(k) * nb_subfr) > budget)
        {
            k--;
        }

        acelp_cfg->fixed_cdk_index[0] = k;
        bitsused = (Word16)ACELP_FIXED_CDK_BITS(k);
        bits += bitsused;

        for (i = 1; i < nb_subfr; i++)
        {
            bits_currsubframe = (Word16)((i * budget + budget) - bitsused * nb_subfr);

            while (k < ACELP_FIXED_CDK_NB - 1 &&
                   (Word16)(ACELP_FIXED_CDK_BITS(k + 1) * nb_subfr) <= bits_currsubframe)
            {
                k++;
            }
            while ((Word16)(ACELP_FIXED_CDK_BITS(k) * nb_subfr) > bits_currsubframe && k > 0)
            {
                k--;
            }

            acelp_cfg->fixed_cdk_index[i] = k;
            bitsused = (Word16)(bitsused + ACELP_FIXED_CDK_BITS(k));
            bits = (Word16)(bits + ACELP_FIXED_CDK_BITS(k));
        }
    }

    remaining_bits = (Word16)(bits_frame - bits);
    if (remaining_bits < 0)
    {
        return -1;
    }

    return bits;
}

/* ============================================================================
 * Unified_weighting_fx
 *
 * LSF weighting for the stochastic codebook.
 * TODO: This is a link-unblock stub.  The float implementation uses tables
 * (Freq_Weight_UV, Freq_Weight_Com, lsf_unified_fit_model_*) that are not
 * present in the FX ROM, so we fall back to a flat perceptual weight of 1.0
 * (Q8).  A later port can re-create the fixed-point tables and mirror the
 * algorithm in lib_enc/qlpc_stoch.c.
 * ============================================================================ */
void Unified_weighting_fx(
    Word32 Bin_Ener_128_fx[],
    Word16 Q_ener,
    const Word16 lsf_fx[],
    Word16 w_fx[],
    const Word16 narrowBand,
    const Word16 unvoiced,
    const Word32 sr_core,
    const Word16 order)
{
    Word16 i;
    for (i = 0; i < order; i++)
    {
        w_fx[i] = 256; /* Q8 = 1.0 */
    }

    (void)Bin_Ener_128_fx;
    (void)Q_ener;
    (void)lsf_fx;
    (void)narrowBand;
    (void)unvoiced;
    (void)sr_core;
}
