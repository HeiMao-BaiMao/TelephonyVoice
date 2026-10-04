/* File driver for local synthetic differential tests, not a shipped codec CLI. */
#include "evs_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    EVS_EncOptions opts;
    FILE *in, *out, *profile = NULL;
    EVS_Encoder *enc = NULL;
    EVS_Decoder *dec = NULL;
    short pcm[960];
    unsigned char packet[6000];
    int sr, br, bw, encoding, n, used, count = 0;
    if (argc != 12 && argc != 13) return 2;
    if (argc == 13 && !(profile = fopen(argv[12], "rb"))) return 12;
    encoding = strcmp(argv[1], "encode") == 0;
    sr = atoi(argv[2]); br = atoi(argv[3]); bw = atoi(argv[4]);
    evs_enc_options_init(&opts);
    opts.dtx_enable = atoi(argv[5]); opts.dtx_sid_interval = atoi(argv[6]);
    opts.rf_enable = atoi(argv[7]); opts.rf_fec_offset = atoi(argv[8]);
    opts.amr_wb_io = atoi(argv[9]);
    in = fopen(argv[10], "rb"); out = fopen(argv[11], "wb");
    if (!in || !out || sr < 8000 || sr > 48000) return 3;
    n = sr / 50;
    if (encoding) enc = evs_enc_create_ex(sr, br, (EVS_Bandwidth)bw, &opts);
    else dec = evs_dec_create_ex(sr, br, opts.amr_wb_io);
    if ((!enc && encoding) || (!dec && !encoding)) return 4;
    for (;;) {
        if (encoding) {
            if (fread(pcm, sizeof(short), (size_t)n, in) != (size_t)n) break;
            if (profile) {
                int32_t rate;
                int io;
                if (fread(&rate, sizeof(rate), 1, profile) != 1) return 13;
                io = rate == 6600 || rate == 8850 || rate == 12650 || rate == 14250 ||
                    rate == 15850 || rate == 18250 || rate == 19850 || rate == 23050 || rate == 23850;
                opts.amr_wb_io = io;
                opts.sc_vbr_enable = rate == 5900;
                if (evs_enc_reconfigure(enc, rate, (EVS_Bandwidth)bw, &opts) != EVS_OK) return 14;
            }
            if (evs_enc_process(enc, pcm, n, packet, sizeof(packet), &used) != EVS_OK) return 5;
            if (fwrite(packet, 1, (size_t)used, out) != (size_t)used) return 6;
        } else {
            uint16_t bits;
            if (fread(packet, 1, 4, in) != 4) break;
            memcpy(&bits, packet + 2, 2);
            if (bits > 2560 || fread(packet + 4, 2, bits, in) != bits) return 7;
            if (evs_dec_process(dec, packet, 4 + 2 * bits, pcm, &used) != EVS_OK) return 8;
            if (used != n || fwrite(pcm, sizeof(short), (size_t)used, out) != (size_t)used) return 9;
        }
        ++count;
    }
    if (ferror(in) || ferror(out)) return 10;
    fclose(in); fclose(out); if (profile) fclose(profile);
    evs_enc_destroy(enc); evs_dec_destroy(dec);
    return count ? 0 : 11;
}
