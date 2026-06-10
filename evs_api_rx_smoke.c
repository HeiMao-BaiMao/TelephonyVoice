// EVS JBM API smoke executable (experimental, float EVS only)
//
// Proves the parent-repo evs_api_rx adapter can receive compact AU data
// produced by the existing evs_enc_process G.192 short-stream output.
// It uses only public parent-repo APIs and is built only when
// TELEPHONY_USE_EVS_JBM=ON.

#include "evs_api.h"
#include "evs_api_rx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMOKE_NUM_FRAMES   50
#define SMOKE_DRAIN_TICKS  20
#define SMOKE_MAX_AU_BYTES 320
#define SMOKE_MAX_AU_BITS  2560
#define G192_BIT1          0x0081u

static int fail(const char* msg) {
    fprintf(stderr, "evs_api_rx_smoke: %s\n", msg);
    return 2;
}

static void fill_square_frame(short* pcm, int n_samples, int phase) {
    int sign = ((phase & 1) ? 1 : -1);
    for (int i = 0; i < n_samples; ++i) {
        pcm[i] = (short)(sign * 32000);
        if (((i + phase) % 40) == 39) {
            sign = -sign;
        }
    }
}

static void accumulate_energy(const short* pcm, int n_samples, unsigned long long* energy) {
    for (int i = 0; i < n_samples; ++i) {
        int v = (int)pcm[i];
        *energy += (unsigned long long)(v < 0 ? -v : v);
    }
}

// Convert the G.192 short-stream returned by evs_enc_process into the compact
// MSB-first EVS AU bytes expected by evs_rx_jbm_feed_frame.
static int g192_to_compact_au(const unsigned char* bitstream,
                              int bitstream_used,
                              unsigned char* compact,
                              int compact_capacity) {
    if (!bitstream || !compact) return -1;
    if (bitstream_used < (int)(2 * sizeof(unsigned short))) return -1;

    const unsigned short* words = (const unsigned short*)bitstream;
    int nwords = bitstream_used / (int)sizeof(unsigned short);
    unsigned int nb_bits = (unsigned int)words[1];
    if (nb_bits < 1u || nb_bits > SMOKE_MAX_AU_BITS) return -1;
    if (nwords < 2 + (int)nb_bits) return -1;

    int compact_bytes = (int)((nb_bits + 7u) / 8u);
    if (compact_bytes > SMOKE_MAX_AU_BYTES) return -1;
    if (compact_capacity < compact_bytes) return -1;

    memset(compact, 0, (size_t)compact_bytes);
    for (unsigned int i = 0; i < nb_bits; ++i) {
        if (words[2u + i] != G192_BIT1) continue;
        int byte_index = (int)(i >> 3);
        int bit_index = 7 - (int)(i & 0x7u);
        compact[byte_index] |= (unsigned char)(1u << bit_index);
    }

    return (int)nb_bits;
}

int main(void) {
    const int sample_rate_hz = 16000;
    const int frame_size = evs_frame_size(sample_rate_hz);
    const int bitrate_bps = EVS_BR_13200;
    const EVS_Bandwidth bw = EVS_WB;

    int rc = 1;
    int max_bs_bytes = evs_max_bitstream_bytes(sample_rate_hz);
    unsigned char* bitstream = (unsigned char*)calloc(1, (size_t)max_bs_bytes);
    short* pcm_in = (short*)calloc((size_t)frame_size, sizeof(short));
    short* pcm_out = (short*)calloc((size_t)frame_size, sizeof(short));
    if (!bitstream || !pcm_in || !pcm_out) {
        rc = fail("allocation failure");
        goto cleanup_no_handles;
    }

    EVS_Encoder* enc = evs_enc_create(sample_rate_hz, bitrate_bps, bw);
    if (!enc) {
        rc = fail("evs_enc_create failed");
        goto cleanup_no_handles;
    }

    EVS_RxJbm* rx = evs_rx_jbm_create(sample_rate_hz, bitrate_bps, 0);
    if (!rx) {
        rc = fail("evs_rx_jbm_create failed");
        evs_enc_destroy(enc);
        goto cleanup_no_handles;
    }

    unsigned char compact[SMOKE_MAX_AU_BYTES];
    unsigned long long energy = 0;
    int ticks = 0;
    int pull_empty = 0;
    int failed = 0;

    for (int i = 0; i < SMOKE_NUM_FRAMES; ++i) {
        fill_square_frame(pcm_in, frame_size, i);

        int bitstream_used = 0;
        if (evs_enc_process(enc, pcm_in, frame_size,
                            bitstream, max_bs_bytes, &bitstream_used) != EVS_OK) {
            fprintf(stderr, "evs_api_rx_smoke: evs_enc_process failed at frame %d\n", i);
            failed = 1;
            break;
        }

        int nb_bits = g192_to_compact_au(bitstream, bitstream_used,
                                         compact, (int)sizeof(compact));
        if (nb_bits <= 0) {
            fprintf(stderr, "evs_api_rx_smoke: G.192 to compact AU conversion failed at frame %d\n", i);
            failed = 1;
            break;
        }

        unsigned int t_ms = (unsigned int)(i * 20u);
        if (evs_rx_jbm_feed_frame(rx, compact, nb_bits,
                                  (unsigned short)i, (unsigned long)t_ms, t_ms) != EVS_OK) {
            fprintf(stderr, "evs_api_rx_smoke: evs_rx_jbm_feed_frame failed at frame %d\n", i);
            failed = 1;
            break;
        }

        int n = 0;
        if (evs_rx_jbm_get_samples(rx, pcm_out, frame_size, t_ms, &n) != EVS_OK) {
            fprintf(stderr, "evs_api_rx_smoke: evs_rx_jbm_get_samples failed at frame %d\n", i);
            failed = 1;
            break;
        }
        if (n == 0) {
            pull_empty++;
        } else {
            accumulate_energy(pcm_out, n, &energy);
        }
        ticks++;
    }

    for (int drain = 0; !failed && drain < SMOKE_DRAIN_TICKS; ++drain) {
        int is_empty = evs_rx_jbm_is_empty(rx);
        if (is_empty == 1) break;
        if (is_empty == EVS_ERROR) {
            fprintf(stderr, "evs_api_rx_smoke: evs_rx_jbm_is_empty failed during drain\n");
            failed = 1;
            break;
        }

        int n = 0;
        unsigned int t_ms = (unsigned int)((SMOKE_NUM_FRAMES + drain) * 20u);
        if (evs_rx_jbm_get_samples(rx, pcm_out, frame_size, t_ms, &n) != EVS_OK) {
            fprintf(stderr, "evs_api_rx_smoke: evs_rx_jbm_get_samples failed during drain\n");
            failed = 1;
            break;
        }
        if (n == 0) {
            pull_empty++;
        } else {
            accumulate_energy(pcm_out, n, &energy);
        }
        ticks++;
    }

    int fec_offset = 0;
    int fec_hi = 0;
    if (evs_rx_jbm_get_fec_offset(rx, &fec_offset, &fec_hi) != EVS_OK) {
        fprintf(stderr, "evs_api_rx_smoke: evs_rx_jbm_get_fec_offset failed\n");
        failed = 1;
    }

    if (!failed && energy == 0ULL) {
        fprintf(stderr, "evs_api_rx_smoke: decoded sample energy is zero\n");
        failed = 1;
    }

    if (!failed) {
        printf("EVSJbmSmoke: OK (frames=%d, ticks=%d, empty_pulls=%d, energy=%llu, fec_offset=%d, fec_hi=%d)\n",
               SMOKE_NUM_FRAMES, ticks, pull_empty, energy, fec_offset, fec_hi);
        rc = 0;
    }

    evs_rx_jbm_destroy(rx);
    evs_enc_destroy(enc);

cleanup_no_handles:
    free(bitstream);
    free(pcm_in);
    free(pcm_out);
    return rc;
}
