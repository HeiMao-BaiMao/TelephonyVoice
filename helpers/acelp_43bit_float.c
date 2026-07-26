/* ============================================================================
 *  43-bit EVS algebraic-codebook indexing / de-indexing.
 *
 *  The self-contained float ACELP helper (acelp_float_wrap.c) pulls in
 *  enc_acelp.c and dec_acelp.c.  Those sources reference the 43-bit special
 *  cases E_ACELP_code43bit() and D_ACELP_decode_43bit(), which live upstream
 *  in lib_enc/cod4t64.c and lib_dec/dec4t64.c.  To avoid dragging in the
 *  whole of those files (and their duplicate public symbols), the small set
 *  of functions and tables needed for the 43-bit path is reproduced here in
 *  plain C.
 *
 *  This file is #include'd by helpers/acelp_float_wrap.c after the symbols
 *  E_ACELP_code43bit / D_ACELP_decode_43bit have been macro-renamed, so the
 *  definitions below are reached through the renamed names.
 * ============================================================================ */

#include <math.h>
#include <string.h>

/* Use local names so we do not clash with the cnst.h macros that
 * enc_acelp.c / dec_acelp.c include later. */
#define ACELP43_NB_TRACK   4
#define ACELP43_NB_POS     16
#define ACELP43_L_SUBFR    64

/* ---------------------------------------------------------------------------
 *  ROM tables for pulse indexing (shared between encoder and decoder).
 * --------------------------------------------------------------------------- */

static const int PI_select_table[23][8] =
{
    {1,     0,     0,     0,     0,     0,     0,       0},
    {1,     1,     0,     0,     0,     0,     0,       0},
    {1,     2,     1,     0,     0,     0,     0,       0},
    {1,     3,     3,     1,     0,     0,     0,       0},
    {1,     4,     6,     4,     1,     0,     0,       0},
    {1,     5,    10,    10,     5,     1,     0,       0},
    {1,     6,    15,    20,    15,     6,     1,       0},
    {1,     7,    21,    35,    35,    21,     7,       1},
    {1,     8,    28,    56,    70,    56,    28,       8},
    {1,     9,    36,    84,   126,   126,    84,      36},
    {1,    10,    45,   120,   210,   252,   210,     120},
    {1,    11,    55,   165,   330,   462,   462,     330},
    {1,    12,    66,   220,   495,   792,   924,     792},
    {1,    13,    78,   286,   715,  1287,  1716,    1716},
    {1,    14,    91,   364,  1001,  2002,  3003,    3432},
    {1,    15,   105,   455,  1365,  3003,  5005,    6435},
    {1,    16,   120,   560,  1820,  4368,  8008,   11440},
    {1,    17,   136,   680,  2380,  6188, 12376,   19448},
    {1,    18,   153,   816,  3060,  8568, 18564,   31824},
    {1,    19,   171,   969,  3876, 11628, 27132,   50388},
    {1,    20,   190,  1140,  4845, 15504, 38760,   77520},
    {1,    21,   210,  1330,  5985, 20349, 54264,  116280},
    {1,    22,   231,  1540,  7315, 26334, 74613, 1705444}
};

static const int PI_offset[8][8] =
{
    {0,   0,    0,    0,    0,    0,    0,    0},
    {0,   2,    0,    0,    0,    0,    0,    0},
    {0,   5,   25,    0,    0,    0,    0,    0},
    {0,   9,   63,  219,    0,    0,    0,    0},
    {0,  14,  126,  636, 1896,    0,    0,    0},
    {0,  20,  220, 1360, 6160,15400,    0,    0},
    {0,  27,  351, 2565,13563, 39951, 85653,   0},
    {0,  35,  525, 4389,26334, 88938,224034, 427266}
};

static const short PI_factor[7] = {0, 0, 120, 560, 1820, 4368, 8008};

/* ---------------------------------------------------------------------------
 *  Encoder helper functions.
 * --------------------------------------------------------------------------- */

static short quant_1p_N1(const short pos, const short N)
{
    short mask = (short)((1 << N) - 1);
    short index = (short)(pos & mask);
    if ((pos & ACELP43_NB_POS) != 0)
    {
        index = (short)(index + (1 << N));
    }
    return index;
}

static short quant_2p_2N1(const short pos1, const short pos2, const short N)
{
    short mask = (short)((1 << N) - 1);
    short index;

    if (((pos2 ^ pos1) & ACELP43_NB_POS) == 0)
    {
        if ((pos1 - pos2) <= 0)
        {
            index = (short)(((pos1 & mask) << N) + (pos2 & mask));
        }
        else
        {
            index = (short)(((pos2 & mask) << N) + (pos1 & mask));
        }
        if ((pos1 & ACELP43_NB_POS) != 0)
        {
            index = (short)(index + (1 << (2 * N)));
        }
    }
    else
    {
        if (((pos1 & mask) - (pos2 & mask)) <= 0)
        {
            index = (short)(((pos2 & mask) << N) + (pos1 & mask));
            if ((pos2 & ACELP43_NB_POS) != 0)
            {
                index = (short)(index + (1 << (2 * N)));
            }
        }
        else
        {
            index = (short)(((pos1 & mask) << N) + (pos2 & mask));
            if ((pos1 & ACELP43_NB_POS) != 0)
            {
                index = (short)(index + (1 << (2 * N)));
            }
        }
    }
    return index;
}

static int pre_process(const float v[], short pos_vector[], int pos_vector_num[], int *pulse_pos_num)
{
    int j = 0;
    int sign = 0;
    int k;

    for (k = 0; k < ACELP43_L_SUBFR; k += ACELP43_NB_TRACK)
    {
        if (v[k] != 0.0f)
        {
            pos_vector[j] = (short)(k >> 2);
            pos_vector_num[j] = (int)fabsf(v[k]);
            if (v[k] > 0.0f)
            {
                sign = sign << 1;
            }
            else
            {
                sign = (sign << 1) + 1;
            }
            j++;
        }
    }
    *pulse_pos_num = j;
    return sign;
}

static int fcb_encode_position(short pos_vector[], int n, int pos_num, int flag)
{
    int i;
    int mmm1 = PI_select_table[n][pos_num] - 1;
    int temp2 = pos_num;

    if (flag)
    {
        for (i = 0; i < pos_num; i++)
        {
            mmm1 -= PI_select_table[n - pos_vector[i] - 1][temp2--];
        }
    }
    else
    {
        for (i = 0; i < pos_num; i++)
        {
            mmm1 -= PI_select_table[n - pos_vector[i] - 1][temp2--];
            n--;
        }
    }
    return mmm1;
}

static int fcb_encode_cl(int buffer[], int pulse_num, int pos_num)
{
    int i;
    int temp1 = pos_num + pulse_num - 1;
    int temp2 = pulse_num;
    int k = PI_select_table[temp1][pulse_num] - 1;
    temp1--;
    for (i = 0; i < pulse_num; i++)
    {
        k -= PI_select_table[temp1 - buffer[i]][temp2--];
        temp1--;
    }
    return k;
}

static int fcb_encode_class(int sector_6p_num[], int pulse_num, int pulse_pos_num)
{
    int i, j, k;
    int mn9_offset = 0;
    int vector_class[6];

    if (pulse_pos_num < pulse_num)
    {
        int *vector_class_ptr = vector_class;
        for (i = 0; i < pulse_pos_num; i++)
        {
            for (j = 0; j < (sector_6p_num[i] - 1); j++)
            {
                *vector_class_ptr++ = i;
            }
        }
        k = fcb_encode_cl(vector_class, pulse_num - pulse_pos_num, pulse_pos_num);
        for (i = 0; i < k; i++)
        {
            mn9_offset += PI_factor[pulse_pos_num];
        }
    }
    return mn9_offset;
}

static int fcb_encode_PI(const float v[], int pulse_num)
{
    short sector_p[7];
    int pulse_pos_num;
    int sector_p_num[7];
    int code_index;
    int sign;

    sign = pre_process(v, sector_p, sector_p_num, &pulse_pos_num);
    code_index = fcb_encode_position(sector_p, 16, pulse_pos_num, 1);
    code_index += fcb_encode_class(sector_p_num, pulse_num, pulse_pos_num);
    code_index = PI_offset[pulse_num][pulse_num + 1 - pulse_pos_num]
                 + (code_index << pulse_pos_num) + sign;
    return code_index;
}

/* ---------------------------------------------------------------------------
 *  43-bit encoder entry point.
 * --------------------------------------------------------------------------- */

short E_ACELP_code43bit(const float code[], long unsigned *ps, int *p, unsigned short idxs[])
{
    int track;
    int ind[32];
    int tmp;
    int joint_index;
    const int joint_offset = 3611648;
    short saved_bits = 0;

    for (track = 0; track < 2; track++)
    {
        ps[track] = (long unsigned)fcb_encode_PI(code + track, 3);
        p[track] = 3;
    }

    for (track = 2; track < ACELP43_NB_TRACK; track++)
    {
        int j = track * 8;   /* NPMAXPT == 8 */
        int k;
        for (k = track; k < ACELP43_L_SUBFR; k += ACELP43_NB_TRACK)
        {
            if (code[k] != 0.0f)
            {
                tmp = k >> 2;
                if (code[k] < 0.0f)
                {
                    tmp += 16;
                }
                if (fabsf(code[k]) > 1.0f)
                {
                    ind[j] = tmp;
                    ind[j + 1] = tmp;
                    break;
                }
                else
                {
                    ind[j] = tmp;
                    j++;
                }
            }
        }
        k = track * 8;
        ps[track] = (long unsigned)quant_2p_2N1((short)ind[k], (short)ind[k + 1], 4);
        p[track] = 2;
    }

    joint_index = (int)(ps[0] * 5472 + ps[1]);
    if (joint_index >= joint_offset)
    {
        joint_index += joint_offset;
    }
    else
    {
        saved_bits += 1;
    }

    idxs[0] = (unsigned short)(((ps[2] << 9) + ps[3]) & 0xffff);
    idxs[1] = (unsigned short)(((joint_index << 2) + (ps[2] >> 7)) & 0xffff);
    idxs[2] = (unsigned short)(joint_index >> 14);

    return saved_bits;
}

/* ---------------------------------------------------------------------------
 *  Decoder helper functions.
 * --------------------------------------------------------------------------- */

static void add_pulses(const short pos[], const short nb_pulse, const short track, float code[])
{
    short k;
    for (k = 0; k < nb_pulse; k++)
    {
        short i = (short)(((pos[k] & (ACELP43_NB_POS - 1)) * ACELP43_NB_TRACK) + track);
        if ((pos[k] & ACELP43_NB_POS) == 0)
        {
            code[i] += 1.0f;
        }
        else
        {
            code[i] -= 1.0f;
        }
    }
}

static void dec_1p_N1(const long index, const short N, const short offset, short pos[])
{
    long mask = ((1L << N) - 1);
    short pos1 = (short)((index & mask) + offset);
    short i = (short)((index >> N) & 1);
    if (i == 1)
    {
        pos1 = (short)(pos1 + ACELP43_NB_POS);
    }
    pos[0] = pos1;
}

static void dec_2p_2N1(const long index, const short N, const short offset, short pos[])
{
    long mask = ((1L << N) - 1);
    short pos1 = (short)(((index >> N) & mask) + offset);
    short i = (short)((index >> (2 * N)) & 1);
    short pos2 = (short)((index & mask) + offset);

    if ((pos2 - pos1) < 0)
    {
        if (i == 1)
        {
            pos1 = (short)(pos1 + ACELP43_NB_POS);
        }
        else
        {
            pos2 = (short)(pos2 + ACELP43_NB_POS);
        }
    }
    else
    {
        if (i == 1)
        {
            pos1 = (short)(pos1 + ACELP43_NB_POS);
            pos2 = (short)(pos2 + ACELP43_NB_POS);
        }
    }

    pos[0] = pos1;
    pos[1] = pos2;
}

static int fcb_decode_class_all_p(int *code_index, short sector_6p_num[], int pulse_num, int *pos_num)
{
    int i, j, k;
    int mn9;
    int pulse_pos_num;

    for (i = 1; i <= pulse_num; i++)
    {
        if ((*code_index) < PI_offset[pulse_num][i])
        {
            break;
        }
    }

    (*code_index) -= PI_offset[pulse_num][i - 1];

    pulse_pos_num = pulse_num - i + 2;
    j = (*code_index) >> pulse_pos_num;

    k = j / PI_select_table[16][pulse_pos_num];
    mn9 = j - k * PI_select_table[16][pulse_pos_num];

    if ((pulse_pos_num < pulse_num) && (pulse_pos_num > 1))
    {
        for (i = 0; i < pulse_pos_num; i++)
        {
            sector_6p_num[i] = 1;
        }
        sector_6p_num[k]++;
    }
    else
    {
        if (pulse_pos_num == 1)
        {
            sector_6p_num[0] = (short)pulse_num;
        }
        else
        {
            for (i = 0; i < pulse_num; i++)
            {
                sector_6p_num[i] = 1;
            }
        }
    }

    *pos_num = pulse_pos_num;
    return mn9;
}

static void fcb_decode_position(int index, short pos_vector[], int pos_num)
{
    int i;
    int k = index;
    int l = 0;
    int temp = pos_num;

    for (i = 0; i < pos_num - 1; i++)
    {
        k = PI_select_table[16 - l][temp] - k;
        for (; PI_select_table[16 - l][temp] >= k; l += 2);
        if (k > PI_select_table[17 - l][temp])
        {
            l--;
        }
        k = PI_select_table[17 - l][temp] - k;
        pos_vector[i] = (short)(l - 1);
    }
    pos_vector[i] = (short)(l + k);
}

static int fcb_decode_PI(int code_index, short sector_6p[], int pulse_num)
{
    int i, l;
    int mn9;
    int pulse_pos_num;
    short sector_6p_temp[7];
    short sector_6p_num_temp[7];
    short *sector_6p_ptr0;
    short *sector_6p_ptr1;

    mn9 = fcb_decode_class_all_p(&code_index, sector_6p_num_temp, pulse_num, &pulse_pos_num);
    fcb_decode_position(mn9, sector_6p_temp, pulse_pos_num);

    for (i = pulse_pos_num - 1; i >= 0; i--)
    {
        sector_6p_temp[i] = (short)(sector_6p_temp[i] + ((code_index & 0x1) << 4));
        code_index = code_index >> 1;
    }

    sector_6p_ptr0 = &sector_6p[pulse_num];
    sector_6p_ptr1 = &sector_6p_temp[pulse_pos_num];
    for (i = 0; i < pulse_pos_num; i++)
    {
        sector_6p_ptr1--;
        for (l = 0; l < sector_6p_num_temp[pulse_pos_num - 1 - i]; l++)
        {
            *--sector_6p_ptr0 = *sector_6p_ptr1;
        }
    }

    return pulse_pos_num;
}

/* ---------------------------------------------------------------------------
 *  43-bit decoder entry point.
 * --------------------------------------------------------------------------- */

void D_ACELP_decode_43bit(unsigned short idxs[], float code[], int *pulsestrack)
{
    int ps[8];
    short pos[7];
    int joint_index;
    const int joint_offset = 3611648;

    memset(code, 0, sizeof(float) * ACELP43_L_SUBFR);

    ps[3] = (int)(idxs[0] & 0x1ff);
    ps[2] = (int)(((idxs[1] & 3) << 7) + (idxs[0] >> 9));
    joint_index = (int)(((idxs[2] << 16) + idxs[1]) >> 2);

    if (joint_index >= joint_offset)
    {
        joint_index -= joint_offset;
    }

    ps[0] = joint_index / 5472;
    ps[1] = joint_index - ps[0] * 5472;

    fcb_decode_PI(ps[0], pos, 3);
    add_pulses(pos, pulsestrack[0], 0, code);
    fcb_decode_PI(ps[1], pos, 3);
    add_pulses(pos, pulsestrack[1], 1, code);

    dec_2p_2N1((long)ps[2], 4, 0, pos);
    add_pulses(pos, pulsestrack[2], 2, code);
    dec_2p_2N1((long)ps[3], 4, 0, pos);
    add_pulses(pos, pulsestrack[3], 3, code);
}
