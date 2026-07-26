/*
 * Thin wrapper that compiles the upstream float ACELP algebraic codebook
 * search/indexing sources into a self-contained object.  External symbols are
 * renamed so they do not clash with the fixed-point stubs, and only the small
 * set of ROM tables / helpers actually used by those sources are supplied here.
 */

/* Rename the functions we want to call from the FX stubs. */
#define E_ACELP_4tsearch   evs_fx_E_ACELP_4tsearch
#define E_ACELP_4tsearchx  evs_fx_E_ACELP_4tsearchx
#define E_ACELP_indexing   evs_fx_E_ACELP_indexing
#define D_ACELP_indexing   evs_fx_D_ACELP_indexing

/* The 43-bit special cases are supplied by acelp_43bit_float.c (see below). */
#define E_ACELP_code43bit  evs_fx_E_ACELP_code43bit
#define D_ACELP_decode_43bit evs_fx_D_ACELP_decode_43bit

/* Rename tables that collide with the fixed-point ROM names. */
#define tipos              evs_float_tipos
#define pulsestostates     evs_float_pulsestostates
#define low_len            evs_float_low_len
#define low_mask           evs_float_low_mask
#define indx_fact          evs_float_indx_fact
#define index_len          evs_float_index_len
#define index_mask         evs_float_index_mask
#define PulseConfTable     evs_float_PulseConfTable

#include "acelp_43bit_float.c"

/* Stubs for helpers only referenced by dead code (E_ACELP_innovative_codebook).
 * Keeping them empty lets /OPT:REF discard that code path without dragging in
 * the rest of the float encoder library. */
void mvr2r(const float x[], float y[], const short n)
{
    short i;
    if (n <= 0) return;
    if (y < x) for (i = 0; i < n; i++) y[i] = x[i];
    else       for (i = n - 1; i >= 0; i--) y[i] = x[i];
}

void corr_xh(const float *x, float *y, const float *h, const int L_subfr)
{
    int i;
    (void)x; (void)h;
    for (i = 0; i < L_subfr; i++) y[i] = 0.0f;
}

void updt_tar(const float *x, float *x2, const float *y, const float gain, const short L)
{
    int i;
    (void)x; (void)y; (void)gain;
    for (i = 0; i < L; i++) x2[i] = 0.0f;
}

void E_ACELP_toeplitz_mul(float R[], float c[], float d[])
{
    int i;
    (void)R; (void)c;
    for (i = 0; i < 64; i++) d[i] = 0.0f;
}

void cb_shape(const short preemphFlag, const short pitchFlag, const short scramblingFlag,
              const short formantFlag, const short formantTiltFlag, const float g1,
              const float g2, const float *p_Aq, float *code, const float tilt_code,
              const float pt_pitch)
{
    (void)preemphFlag; (void)pitchFlag; (void)scramblingFlag; (void)formantFlag;
    (void)formantTiltFlag; (void)g1; (void)g2; (void)p_Aq; (void)code;
    (void)tilt_code; (void)pt_pitch;
}

/* Minimal copies of the ROM tables / helpers referenced by the sources above. */
const short tipos[40] =
{
    0, 1, 2, 3,
    1, 2, 3, 0,
    2, 3, 0, 1,
    3, 0, 1, 2,
    0, 1, 2, 3,
    1, 2, 3, 0,
    2, 3, 0, 1,
    3, 0, 1, 2,
    0, 1, 2, 3,
    1, 2, 3, 0
};

const long unsigned pulsestostates[17][9] =
{
    {0,0,0,0,0,0,0,0,0},
    {2,2,2,2,2,2,2,2,2},
    {5,6,6,6,6,6,6,6,6},
    {9,12,13,13,13,13,13,13,13},
    {14,20,23,24,24,24,24,24,24},
    {20,30,36,38,39,39,39,39,39},
    {27,42,52,57,59,60,60,60,60},
    {35,56,72,81,86,88,89,89,89},
    {44,72,96,111,120,125,127,128,128},
    {54,90,123,146,161,170,175,177,178},
    {65,110,155,188,213,229,239,244,246},
    {77,132,192,239,277,303,320,330,335},
    {90,156,233,296,352,393,421,440,450},
    {104,182,280,365,443,503,550,583,603},
    {119,210,333,448,556,644,714,768,803},
    {135,240,392,542,689,812,918,1001,1058},
    {152,272,456,646,848,1021,1182,1314,1402}
};

const int low_len[10]   = { 0, 0, 8, 5, 7, 11, 13, 15, 16, 16 };
const int low_mask[10]  = { 0, 0, 255, 31, 127, 2047, 8191, 32767, 65535, 65535 };
const int indx_fact[10] = { 0, 0, 2, 172, 345, 140, 190, 223, 463, 1732 };
const int index_len[3]  = { 0, 5, 9 };
const int index_mask[3] = { 0, 31, 511 };

/* Dummy table - only referenced by dead code (E_ACELP_4t) that is discarded. */
#include "stat_com.h"
const PulseConfig PulseConfTable[1] = { { 0 } };

/* Utility helpers. */
void set_i(int y[], const int a, const short N)
{
    short i;
    for (i = 0; i < N; i++) y[i] = a;
}

void set_f(float y[], const float a, const short N)
{
    short i;
    for (i = 0; i < N; i++) y[i] = a;
}

void longadd(unsigned short a[], unsigned short b[], int lena, int lenb)
{
    int h;
    long carry = 0;
    for (h = 0; h < lenb; h++)
    {
        carry += ((unsigned long)a[h]) + ((unsigned long)b[h]);
        a[h] = (unsigned short)carry;
        carry = carry >> 16;
    }
    for (; h < lena; h++)
    {
        carry = ((unsigned long)a[h]) + carry;
        a[h] = (unsigned short)carry;
        carry = carry >> 16;
    }
}

void longshiftright(unsigned short a[], int b, unsigned short d[], int lena, int lend)
{
    int intb, fracb, fracb_u, k;
    intb = b >> 4;
    a += intb;
    lena -= intb;
    fracb = b & 0xF;
    if (fracb)
    {
        fracb_u = 16 - fracb;
        for (k = 0; k < lena - 1; k++)
            d[k] = ((a[k] >> fracb) | (a[k + 1] << fracb_u)) & 0xFFFF;
        d[k] = (a[k] >> fracb);
        k++;
    }
    else
    {
        for (k = 0; k < lena; k++) d[k] = a[k];
    }
    for (; k < lend; k++) d[k] = 0;
}

void longshiftleft(unsigned short a[], int b, unsigned short d[], int len)
{
    int intb, fracb, fracb_l, k = len - 1;
    intb = b >> 4;
    fracb = b & 0xF;
    if (fracb)
    {
        fracb_l = 16 - fracb;
        for (; k > intb; k--)
            d[k] = (a[k - intb] << fracb) | (a[k - intb - 1] >> fracb_l);
        d[k] = (a[k - intb] << fracb);
        k--;
    }
    else
    {
        for (; k >= intb; k--) d[k] = a[k - intb];
    }
    for (; k >= 0; k--) d[k] = 0;
}

/* Pull in only the float codebook search/indexing sources. */
#include "enc_acelp.c"
#include "enc_acelpx.c"
#include "dec_acelp.c"
