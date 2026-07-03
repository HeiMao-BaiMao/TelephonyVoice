// Smoke test for the TelephonyDSP static library.
//
// Part 1 sweeps every EraMode reachable in the current build configuration
// over a synthetic 440 Hz tone and checks that the processor emits finite,
// non-silent audio.
//
// Part 2 (non-distribution builds only) round-trips the raw EVS C API with
// DTX enabled, covering active speech frames, the SID / NO_DATA zero-length
// frame path, and the packet-loss concealment entry point. This directly
// exercises the in-memory G.192 serialisation in evs_api.c.
//
// Exit code 0 = all checks passed; non-zero = at least one failure.

#define NOMINMAX
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dsp/SignalProcessor.h"
#ifndef TELEPHONY_DISTRIBUTION_BUILD
#include "evs_api.h"
#endif

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// ---------------------------------------------------------------------------
// Part 1: SignalProcessor mode sweep
// ---------------------------------------------------------------------------
static void runModeSweep() {
    using namespace TelephonyDSP;

    struct ModeCase { EraMode mode; const char* name; };
    const ModeCase modes[] = {
        { EraMode::PSTN_G711,    "PSTN_G711" },
        { EraMode::GSM_FR,       "GSM_FR" },
        { EraMode::AMR_NB_3G,    "AMR_NB_3G" },
        { EraMode::AMR_WB_VOLTE, "AMR_WB_VOLTE" },
        { EraMode::EVS_LIKE,     "EVS_LIKE" },
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        { EraMode::EVS_NATIVE,   "EVS_NATIVE" },
#endif
        { EraMode::Bypass,       "Bypass" },
#if TELEPHONY_EXPERIMENTAL_NETWORK
        { EraMode::OPUS_VOIP,    "OPUS_VOIP" },
#endif
    };

    const int sampleRate = 48000;
    const int blockSize  = 512;
    const int totalSamples = sampleRate * 2; // 2 seconds

    for (const ModeCase& mc : modes) {
        SignalProcessor dsp;
        dsp.setSampleRate(sampleRate);
        dsp.setMode(mc.mode);
        dsp.setSimulateLatency(false);
        // Full wet, unity gain, no artifacts, no loss/degradation.
        dsp.setParameters(1.0f, 0.0f, false, 0.0f, 0.0f, 0.0f);

        std::vector<float> inBlock(blockSize), outBlock(blockSize);
        double tailEnergy = 0.0;
        bool allFinite = true;
        int processed = 0;

        // Speech-like stimulus: harmonic tone with 5 Hz vibrato, gated into
        // 150 ms on / 100 ms off bursts. A steady sine is classified as
        // stationary background by the EVS VAD and the SpeexDSP energy VAD,
        // which sends DTX modes into comfort-noise (near-silent) output and
        // would fail the energy check below for reasons unrelated to a bug.
        double phase = 0.0;
        const double twoPi = 2.0 * 3.14159265358979;

        while (processed < totalSamples) {
            for (int i = 0; i < blockSize; ++i) {
                double t = (double)(processed + i) / sampleRate;
                double freq = 220.0 * (1.0 + 0.1 * std::sin(twoPi * 5.0 * t));
                phase += twoPi * freq / sampleRate;
                double env = (std::fmod(t, 0.25) < 0.15) ? 1.0 : 0.0;
                double s = std::sin(phase) + 0.5 * std::sin(2.0 * phase) + 0.25 * std::sin(3.0 * phase);
                inBlock[i] = (float)(0.4 * env * s);
            }
            float* inPtr = inBlock.data();
            float* outPtr = outBlock.data();
            dsp.process(&inPtr, 1, &outPtr, 1, blockSize);

            for (int i = 0; i < blockSize; ++i) {
                if (!std::isfinite(outBlock[i])) allFinite = false;
                // Only measure the second half so codec/resampler latency
                // and slow onsets cannot fake a silence failure.
                if (processed + i >= totalSamples / 2) {
                    tailEnergy += (double)outBlock[i] * outBlock[i];
                }
            }
            processed += blockSize;
        }

        char label[96];
        std::snprintf(label, sizeof(label), "%s output is finite", mc.name);
        check(allFinite, label);
        std::snprintf(label, sizeof(label), "%s output is not silent (tail energy %.3g)", mc.name, tailEnergy);
        check(tailEnergy > 1e-6, label);
    }
}

// ---------------------------------------------------------------------------
// Part 2: raw EVS C API round-trip with DTX
// ---------------------------------------------------------------------------
#ifndef TELEPHONY_DISTRIBUTION_BUILD
static void runEvsApiRoundTrip(int sampleRate, EVS_Bandwidth bw, const char* bwName) {
    const int bitrate = 13200;
    const int frameSize = evs_frame_size(sampleRate);

    EVS_EncOptions opts;
    evs_enc_options_init(&opts);
    opts.dtx_enable = 1;
    opts.dtx_sid_interval = 0; // variable SID interval

    char label[128];
    EVS_Encoder* enc = evs_enc_create_ex(sampleRate, bitrate, bw, &opts);
    EVS_Decoder* dec = evs_dec_create(sampleRate, bitrate);
    std::snprintf(label, sizeof(label), "EVS %s encoder created (13.2 kbps, DTX on)", bwName);
    check(enc != nullptr, label);
    std::snprintf(label, sizeof(label), "EVS %s decoder created", bwName);
    check(dec != nullptr, label);
    if (!enc || !dec) {
        if (enc) evs_enc_destroy(enc);
        if (dec) evs_dec_destroy(dec);
        return;
    }

    const int maxBytes = evs_max_bitstream_bytes(sampleRate);
    std::vector<unsigned char> payload(maxBytes);
    std::vector<short> pcmIn(frameSize), pcmOut(frameSize);

    // 40 frames tone, 60 frames digital silence (must push the encoder into
    // DTX and emit short SID / zero-length NO_DATA frames), 20 frames tone.
    const int toneA = 40, silence = 60, toneB = 20;
    const int totalFrames = toneA + silence + toneB;
    const int activeFrameBytes = (2 + bitrate / 50) * 2; // G.192 words for 13.2 kbps

    bool allEncodeOk = true, allDecodeOk = true, allLengthOk = true;
    bool sawShortFrame = false;
    double toneEnergy = 0.0;
    int sampleIndex = 0;

    for (int f = 0; f < totalFrames; ++f) {
        bool active = (f < toneA) || (f >= toneA + silence);
        for (int i = 0; i < frameSize; ++i, ++sampleIndex) {
            double t = (double)sampleIndex / sampleRate;
            pcmIn[i] = active ? (short)(10000.0 * std::sin(2.0 * 3.14159265358979 * 330.0 * t)) : 0;
        }

        int used = 0;
        if (evs_enc_process(enc, pcmIn.data(), frameSize, payload.data(), maxBytes, &used) != EVS_OK) {
            allEncodeOk = false;
            continue;
        }
        // In the second half of the silence block DTX must have engaged,
        // so frames there have to be shorter than an active 13.2 kbps frame.
        if (f >= toneA + silence / 2 && f < toneA + silence && used < activeFrameBytes) {
            sawShortFrame = true;
        }

        // Drop two frames mid-tone to exercise the concealment path.
        int n = 0;
        int rc;
        if (f == 20 || f == 21) {
            rc = evs_dec_process_lost(dec, pcmOut.data(), &n);
        } else {
            rc = evs_dec_process(dec, payload.data(), used, pcmOut.data(), &n);
        }
        if (rc != EVS_OK) { allDecodeOk = false; continue; }
        if (n != frameSize) allLengthOk = false;

        // Measure decoded energy over the later tone frames only (clear of
        // startup, concealment, and DTX hangover).
        if (f >= 25 && f < toneA) {
            for (int i = 0; i < frameSize; ++i) {
                toneEnergy += (double)pcmOut[i] * pcmOut[i];
            }
        }
    }

    std::snprintf(label, sizeof(label), "EVS %s encode succeeded for all frames", bwName);
    check(allEncodeOk, label);
    std::snprintf(label, sizeof(label), "EVS %s decode succeeded for all frames (incl. SID/NO_DATA)", bwName);
    check(allDecodeOk, label);
    std::snprintf(label, sizeof(label), "EVS %s decode always produced a full 20 ms frame", bwName);
    check(allLengthOk, label);
    std::snprintf(label, sizeof(label), "EVS %s DTX engaged during silence (short SID/NO_DATA frames seen)", bwName);
    check(sawShortFrame, label);
    std::snprintf(label, sizeof(label), "EVS %s decoded tone has energy (%.3g)", bwName, toneEnergy);
    check(toneEnergy > 1e6, label);

    evs_enc_destroy(enc);
    evs_dec_destroy(dec);
}
#endif // !TELEPHONY_DISTRIBUTION_BUILD

int main() {
    runModeSweep();
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    runEvsApiRoundTrip(16000, EVS_WB, "WB 16k");
    // 32 kHz SWB at 13.2 kbps is the plugin's default EVS_NATIVE config.
    runEvsApiRoundTrip(32000, EVS_SWB, "SWB 32k");
#endif
    if (g_failures) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
