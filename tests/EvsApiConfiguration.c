/* Synthetic-only regression coverage for the floating-point EVS wrapper. */
#include "evs_api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "EVS configuration check failed at line %d: %s\n", \
            __LINE__, #condition); return 1; } } while (0)

static int check_validation(void) {
    EVS_EncOptions opts;
    EVS_Bandwidth bw = EVS_FB;
    int br = 128000;
    evs_enc_options_init(&opts);
    opts.dtx_enable = 1;
    CHECK(evs_enc_normalize_config(8000, &br, &bw, &opts) == EVS_ERROR);
    CHECK(br == 128000 && bw == EVS_FB); /* Failure is transactional. */
    CHECK(evs_enc_create_ex(8000, 128000, EVS_FB, &opts) == NULL);
    CHECK(evs_enc_create_ex(16000, 12345, EVS_WB, &opts) == NULL);
    CHECK(evs_enc_create_ex(16000, 13200, (EVS_Bandwidth)-1, &opts) == NULL);
    CHECK(evs_enc_create_ex(16000, 13200, (EVS_Bandwidth)4, &opts) == NULL);
    CHECK(evs_enc_create_ex(44100, 13200, EVS_WB, &opts) == NULL);
    CHECK(evs_enc_create(16000, 5900, EVS_WB) == NULL);
    CHECK(evs_dec_create(16000, 12345) == NULL);
    CHECK(evs_dec_create(44100, 13200) == NULL);
    opts.dtx_sid_interval = 2;
    CHECK(evs_enc_create_ex(16000, 13200, EVS_WB, &opts) == NULL);
    opts.dtx_sid_interval = 0;
    opts.rf_enable = 1;
    opts.rf_fec_offset = 4;
    CHECK(evs_enc_create_ex(16000, 13200, EVS_WB, &opts) == NULL);
    br = 13200; bw = EVS_NB;
    CHECK(evs_enc_normalize_config(16000, &br, &bw, &opts) == EVS_OK);
    CHECK(opts.rf_enable == 0 && opts.rf_fec_offset == 0);
    br = 128000; bw = EVS_FB;
    opts.sc_vbr_enable = 1;
    CHECK(evs_enc_normalize_config(48000, &br, &bw, &opts) == EVS_OK);
    CHECK(br == 5900 && bw == EVS_WB && opts.sc_vbr_enable == 1);
    br = 5900; bw = EVS_FB; opts.sc_vbr_enable = 0;
    CHECK(evs_enc_normalize_config(48000, &br, &bw, &opts) == EVS_OK);
    CHECK(br == 5900 && bw == EVS_WB && opts.sc_vbr_enable == 1);
    br = 13200; bw = EVS_FB; opts.sc_vbr_enable = 0;
    CHECK(evs_enc_normalize_config(48000, &br, &bw, &opts) == EVS_OK);
    CHECK(br == 13200 && bw == EVS_SWB);
    return 0;
}

static int check_round_trip(int sr, int br, EVS_Bandwidth bw,
                            int rf, int sc_vbr, int dtx, int switch_rf) {
    EVS_EncOptions opts;
    EVS_Encoder* enc;
    EVS_Decoder* dec;
    unsigned char stream[6000];
    short input[960], output[960];
    int frame, i, used, decoded, n = sr / 50;
    evs_enc_options_init(&opts);
    opts.dtx_enable = dtx;
    opts.rf_enable = rf;
    opts.sc_vbr_enable = sc_vbr;
    enc = evs_enc_create_ex(sr, br, bw, &opts);
    dec = evs_dec_create(sr, br);
    CHECK(enc != NULL && dec != NULL);
    memset(input, 0, sizeof(input));
    // Insufficient output space consumes a frame but must not retain indices.
    for (i = 0; i < 3; ++i) {
        used = 99;
        CHECK(evs_enc_process(enc, input, n, stream, 0, &used) == EVS_ERROR);
        CHECK(used == 0);
    }
    for (frame = 0; frame < 48; ++frame) {
        /* Exercise active speech-like tones, DTX entry, and resumption. */
        for (i = 0; i < n; ++i) {
            input[i] = (frame >= 12 && frame < 40) ? 0 :
                (short)(10000.0 * sin(6.283185307179586 * 440.0 *
                                      (frame * n + i) / sr));
        }
        if (switch_rf) {
            if (frame == 4 || frame == 24) evs_enc_set_rf(enc, 1, 3, 1);
            if (frame == 10 || frame == 36) evs_enc_set_rf(enc, 0, 0, 1);
            if (frame == 42) evs_enc_set_rf(enc, 1, 4, 1); /* Ignored. */
            if (frame == 44) evs_enc_set_rf(enc, 1, 7, 0);
        }
        used = decoded = 0;
        CHECK(evs_enc_process(enc, input, n, stream, sizeof(stream), &used) == EVS_OK);
        CHECK(used >= 4 && used <= evs_max_bitstream_bytes(sr));
        if (frame == 5 || frame == 39) {
            CHECK(evs_dec_process_lost(dec, output, &decoded) == EVS_OK);
        } else {
            CHECK(evs_dec_process(dec, stream, used, output, &decoded) == EVS_OK);
        }
        CHECK(decoded == n);
    }
    CHECK(evs_dec_process_lost(dec, output, &decoded) == EVS_OK);
    CHECK(decoded == n);
    {
        // Native-only wrapper must reject AMR-WB IO instead of dispatching
        // the packet to the EVS primary decoder with incompatible state.
        unsigned short amr_frame[134];
        memset(amr_frame, 0, sizeof(amr_frame));
        amr_frame[0] = 0x6b21; /* G.192 good-frame sync */
        amr_frame[1] = 132;    /* AMR-WB 6600 bps */
        CHECK(evs_dec_process(dec, (const unsigned char*)amr_frame,
                              sizeof(amr_frame), output, &decoded) == EVS_ERROR);
    }
    evs_enc_destroy(enc);
    evs_dec_destroy(dec);
    return 0;
}

int main(void) {
    static const int rates[] = {8000, 16000, 32000, 48000};
    static const int bitrates[] = {5900, 7200, 8000, 9600, 13200, 16400,
                                  24400, 32000, 48000, 64000, 96000, 128000};
    int s, b, w, rf, count = 0;
    CHECK(check_validation() == 0);
    for (s = 0; s < 4; ++s) for (b = 0; b < 12; ++b)
        for (w = 0; w < 4; ++w) for (rf = 0; rf < 2; ++rf) {
            EVS_EncOptions opts;
            EVS_Bandwidth bw = (EVS_Bandwidth)w;
            int br = bitrates[b];
            evs_enc_options_init(&opts);
            opts.dtx_enable = 1;
            opts.rf_enable = rf;
            if (evs_enc_normalize_config(rates[s], &br, &bw, &opts) != EVS_OK) continue;
            fprintf(stdout, "EVS configuration: %d Hz, %d bps, bandwidth %d, RF %d\n",
                    rates[s], bitrates[b], w, rf);
            fflush(stdout);
            CHECK(check_round_trip(rates[s], bitrates[b], (EVS_Bandwidth)w,
                                   rf, 0, 1, 0) == 0);
            ++count;
        }
    CHECK(check_round_trip(48000, 128000, EVS_FB, 1, 1, 1, 0) == 0);
    CHECK(check_round_trip(16000, 13200, EVS_WB, 0, 0, 1, 1) == 0);
    CHECK(check_round_trip(32000, 13200, EVS_SWB, 1, 0, 1, 1) == 0);
    CHECK(check_round_trip(8000, 13200, EVS_FB, 0, 0, 1, 1) == 0);
    CHECK(check_round_trip(16000, 13200, EVS_NB, 0, 0, 1, 1) == 0);
    CHECK(check_round_trip(8000, 9600, EVS_NB, 0, 0, 0, 0) == 0);
    CHECK(check_round_trip(32000, 16400, EVS_SWB, 0, 0, 0, 0) == 0);
    printf("EVS configuration regression passed: %d matrix combinations plus targeted cases\n", count);
    return 0;
}
