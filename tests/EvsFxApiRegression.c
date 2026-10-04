/* Synthetic boundary tests for the complete-reference fixed-point adapter. */
#include "evs_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
int main(void) {
    EVS_Encoder *a, *b;
    EVS_Decoder *d;
    EVS_EncOptions opts;
    unsigned char pa[6001], pb[6001], malformed[6001];
    short pcm[960] = {0}, output[960];
    int ua, ub, n, i, frame, bits, sid, mode;
    uint16_t bad;
    evs_enc_options_init(&opts);
    CHECK(evs_enc_create(16000, 12345, EVS_WB) == NULL);
    CHECK(evs_dec_create(16000, 12345) == NULL);
    CHECK(evs_dec_create(44100, 13200) == NULL);
    CHECK(evs_dec_create_ex(16000, 13200, 1) == NULL);
    CHECK(evs_enc_reconfigure(NULL, 13200, EVS_WB, NULL) == EVS_ERROR);
    a = evs_enc_create(16000, 13200, EVS_WB);
    b = evs_enc_create(16000, 13200, EVS_WB);
    d = evs_dec_create(16000, 13200);
    CHECK(a && b && d);
    CHECK(evs_dec_process_lost(d, output, &n) == EVS_OK && n == 320);
    for (frame = 0; frame < 12; ++frame) {
        for (i = 0; i < 320; ++i) pcm[i] = frame == 0 ? 0 : (short)(((i + frame * 13) % 79 - 39) * 270);
        /* Invalid reconfiguration and bad RF offsets must leave state alone. */
        CHECK(evs_enc_reconfigure(a, 12345, EVS_WB, NULL) == EVS_ERROR);
        evs_enc_set_rf(a, 1, 4, 0);
        ua = ub = -1;
        CHECK(evs_enc_process(a, pcm, 320, pa + 1, 6000, &ua) == EVS_OK);
        CHECK(evs_enc_process(b, pcm, 320, pb + 1, 6000, &ub) == EVS_OK);
        CHECK(ua == ub && ua == (4 + 264 * 2));
        CHECK(memcmp(pa + 1, pb + 1, (size_t)ua) == 0);
        CHECK(evs_enc_get_last_frame_info(a, &bits, &sid, &mode) == EVS_OK);
        CHECK(bits == 264 && sid == 0 && mode == -1);
        CHECK(evs_dec_process(d, pa + 1, ua, output, &n) == EVS_OK && n == 320);
        memcpy(malformed, pa + 1, (size_t)ua);
        n = 99;
        CHECK(evs_dec_process(d, malformed, ua - 1, output, &n) == EVS_ERROR && n == 0);
        CHECK(evs_dec_process(d, malformed, ua + 1, output, &n) == EVS_ERROR && n == 0);
        bad = 0xffff; memcpy(malformed, &bad, 2);
        CHECK(evs_dec_process(d, malformed, ua, output, &n) == EVS_ERROR && n == 0);
        memcpy(malformed, pa + 1, (size_t)ua); memcpy(malformed + 2, &bad, 2);
        CHECK(evs_dec_process(d, malformed, ua, output, &n) == EVS_ERROR && n == 0);
        memcpy(malformed, pa + 1, (size_t)ua); memcpy(malformed + 4, &bad, 2);
        CHECK(evs_dec_process(d, malformed, ua, output, &n) == EVS_ERROR && n == 0);
    }
    /* Capacity failure consumes exactly one frame and clears indices. */
    ua = 99;
    CHECK(evs_enc_process(a, pcm, 320, pa, 0, &ua) == EVS_ERROR && ua == 0);
    CHECK(evs_enc_process(b, pcm, 320, pb, sizeof(pb), &ub) == EVS_OK);
    CHECK(evs_enc_process(a, pcm, 320, pa, sizeof(pa), &ua) == EVS_OK);
    CHECK(evs_enc_process(b, pcm, 320, pb, sizeof(pb), &ub) == EVS_OK);
    CHECK(ua == ub && memcmp(pa, pb, (size_t)ua) == 0);
    ua = 99;
    CHECK(evs_enc_process(a, pcm, 319, pa, sizeof(pa), &ua) == EVS_ERROR && ua == 0);
    evs_enc_destroy(a); evs_enc_destroy(b); evs_dec_destroy(d);
    /* Multiple lengths in one reused byte buffer cannot leak old frame tails. */
    a = evs_enc_create(48000, 128000, EVS_FB); CHECK(a);
    CHECK(evs_enc_process(a, pcm, 960, pa, sizeof(pa), &ua) == EVS_OK && ua == 5124);
    CHECK(evs_enc_reconfigure(a, 7200, EVS_WB, NULL) == EVS_OK);
    CHECK(evs_enc_process(a, pcm, 960, pa, sizeof(pa), &ua) == EVS_OK && ua == 292);
    evs_enc_destroy(a);
    /* Repeated UI apply preserves adaptive DTX/SID timing. */
    opts.dtx_enable = 1;
    opts.dtx_sid_interval = 0;
    a = evs_enc_create_ex(16000, 13200, EVS_WB, &opts);
    b = evs_enc_create_ex(16000, 13200, EVS_WB, &opts);
    CHECK(a && b);
    for (frame = 0; frame < 120; ++frame) {
        for (i = 0; i < 320; ++i)
            pcm[i] = frame < 20 || frame > 100 ? (short)(((i + frame * 13) % 79 - 39) * 270) : 0;
        CHECK(evs_enc_reconfigure(a, 13200, EVS_WB, &opts) == EVS_OK);
        CHECK(evs_enc_process(a, pcm, 320, pa, sizeof(pa), &ua) == EVS_OK);
        CHECK(evs_enc_process(b, pcm, 320, pb, sizeof(pb), &ub) == EVS_OK);
        CHECK(ua == ub && memcmp(pa, pb, (size_t)ua) == 0);
    }
    evs_enc_destroy(a); evs_enc_destroy(b);
    puts("EVS FX API validation, silence, unaligned buffers, transactional updates and capacity recovery passed");
    return 0;
}
