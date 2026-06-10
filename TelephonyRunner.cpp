#define NOMINMAX
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "TelephonyDSP.h"
#include "wavreader.h"
#include "wavwriter.h"

std::string getOutputFilename(const std::string& input, const std::string& suffix) {
    size_t dot = input.find_last_of(".");
    if (dot == std::string::npos) return input + "." + suffix + ".wav";
    return input.substr(0, dot) + "." + suffix + input.substr(dot);
}

const char* getModeSuffix(TelephonyDSP::EraMode mode) {
    switch (mode) {
        case TelephonyDSP::EraMode::PSTN_G711: return "g711";
        case TelephonyDSP::EraMode::GSM_FR: return "gsm";
        case TelephonyDSP::EraMode::AMR_NB_3G: return "3g";
        case TelephonyDSP::EraMode::AMR_WB_VOLTE: return "volte";
        case TelephonyDSP::EraMode::EVS_LIKE: return "evs_like";
        case TelephonyDSP::EraMode::EVS_NATIVE: return "evs_native";
        case TelephonyDSP::EraMode::Bypass: return "bypass";
#if TELEPHONY_USE_EVS_JBM
        case TelephonyDSP::EraMode::EVS_JBM: return "evs_jbm";
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
        case TelephonyDSP::EraMode::OPUS_VOIP: return "opus_voip";
#endif
        default: return "unknown";
    }
}

void processFile(const std::string& inputFile, TelephonyDSP::EraMode mode,
                 int evsSr, int evsBr, EVS_Bandwidth evsBw) {
    std::string outputFile = getOutputFilename(inputFile, getModeSuffix(mode));
    std::cout << "Processing " << getModeSuffix(mode) << " -> " << outputFile << "..." << std::endl;

    void* wavIn = wav_read_open(inputFile.c_str());
    if (!wavIn) {
        std::cerr << "Error opening input file: " << inputFile << std::endl;
        return;
    }

    int format, channels, sampleRate, bitsPerSample;
    unsigned int dataLength;
    if (!wav_get_header(wavIn, &format, &channels, &sampleRate, &bitsPerSample, &dataLength)) {
        std::cerr << "Error reading WAV header" << std::endl;
        wav_read_close(wavIn);
        return;
    }

    if (bitsPerSample != 16) {
        std::cerr << "Only 16-bit PCM supported. Input is " << bitsPerSample << "-bit." << std::endl;
        wav_read_close(wavIn);
        return;
    }

    void* wavOut = wav_write_open(outputFile.c_str(), sampleRate, 16, channels);
    if (!wavOut) {
        std::cerr << "Error opening output file: " << outputFile << std::endl;
        wav_read_close(wavIn);
        return;
    }

    TelephonyDSP::SignalProcessor dsp;
    dsp.setSampleRate(sampleRate);
    dsp.setMode(mode);
    if (mode == TelephonyDSP::EraMode::EVS_NATIVE) {
        // EVS config driven by CLI flags (defaults: SWB 32 kHz, 13.2 kbps).
        dsp.setEVSConfig(evsSr, evsBr, evsBw);
    }
#if TELEPHONY_USE_EVS_JBM
    if (mode == TelephonyDSP::EraMode::EVS_JBM) {
        // Same CLI-driven config as EVS_NATIVE.
        dsp.setEVSConfig(evsSr, evsBr, evsBw);
    }
#endif
    dsp.setParameters(1.0f, 0.0f, false, 0.0f);
    dsp.setSimulateLatency(false); // Disable artificial 100ms latency for runner

    const int BLOCK_SIZE = 1024;
    std::vector<int16_t> inRaw(BLOCK_SIZE * channels);
    std::vector<int16_t> outRaw(BLOCK_SIZE * channels);
    
    // Deinterleave buffers
    std::vector<std::vector<float>> inputs(channels, std::vector<float>(BLOCK_SIZE));
    std::vector<std::vector<float>> outputs(channels, std::vector<float>(BLOCK_SIZE));
    std::vector<float*> inputPtrs(channels);
    std::vector<float*> outputPtrs(channels);

    for(int i=0; i<channels; ++i) {
        inputPtrs[i] = inputs[i].data();
        outputPtrs[i] = outputs[i].data();
    }

    // 1. Process regular blocks
    while (true) {
        int bytesRead = wav_read_data(wavIn, (unsigned char*)inRaw.data(), BLOCK_SIZE * channels * sizeof(int16_t));
        if (bytesRead <= 0) break;
        int samplesRead = bytesRead / (channels * sizeof(int16_t));

        // Deinterleave
        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < samplesRead; ++i) {
                inputs[ch][i] = inRaw[i * channels + ch] / 32768.0f;
            }
        }

        dsp.process(inputPtrs.data(), channels, outputPtrs.data(), channels, samplesRead);

        // Interleave
        for (int i = 0; i < samplesRead; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                outRaw[i * channels + ch] = (int16_t)std::clamp(outputs[ch][i] * 32767.0f, -32768.0f, 32767.0f);
            }
        }
        wav_write_data(wavOut, (unsigned char*)outRaw.data(), samplesRead * channels * sizeof(int16_t));
    }

    // 2. Flush internal buffers (Smart Flush)
    // Push silence until output drops below threshold or timeout
    // Clear inputs
    for (int ch=0; ch<channels; ++ch) std::fill(inputs[ch].begin(), inputs[ch].end(), 0.0f);
    
    int consecutiveSilentBlocks = 0;
    const int MAX_FLUSH_BLOCKS = 500; // ~10 seconds safety limit
    const int SILENCE_THRESHOLD_BLOCKS = 5; // Stop after ~100ms of silence
    const float SIGNAL_THRESHOLD = 0.002f; // ~ -54dB, ignores codec noise floor

    for (int b = 0; b < MAX_FLUSH_BLOCKS; ++b) {
        dsp.process(inputPtrs.data(), channels, outputPtrs.data(), channels, BLOCK_SIZE);
        
        bool hasSignal = false;
        for (int i = 0; i < BLOCK_SIZE; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                outRaw[i * channels + ch] = (int16_t)std::clamp(outputs[ch][i] * 32767.0f, -32768.0f, 32767.0f);
                if (std::abs(outputs[ch][i]) > SIGNAL_THRESHOLD) hasSignal = true;
            }
        }
        
        if (hasSignal) {
            consecutiveSilentBlocks = 0;
            wav_write_data(wavOut, (unsigned char*)outRaw.data(), BLOCK_SIZE * channels * sizeof(int16_t));
        } else {
            consecutiveSilentBlocks++;
            // Still write silence to preserve timing? Or stop?
            // If we stop writing, the file ends.
            // If the user wants the TAIL, we should write until silence.
            // We write the silence too, to let the fade out complete naturally.
            wav_write_data(wavOut, (unsigned char*)outRaw.data(), BLOCK_SIZE * channels * sizeof(int16_t));
            
            if (consecutiveSilentBlocks >= SILENCE_THRESHOLD_BLOCKS) break;
        }
    }

    wav_read_close(wavIn);
    wav_write_close(wavOut);
}

static bool isValidSampleRate(int sr) {
    return sr == 8000 || sr == 16000 || sr == 32000 || sr == 48000;
}

static bool isValidBitrate(int br) {
    switch (br) {
        case 5900: case 7200: case 8000: case 9600:
        case 13200: case 16400: case 24400: case 32000:
        case 48000: case 64000: case 96000: case 128000:
            return true;
        default:
            return false;
    }
}

static bool parseBandwidth(const char* s, EVS_Bandwidth& out) {
    if (std::strcmp(s, "NB") == 0) { out = EVS_NB;  return true; }
    if (std::strcmp(s, "WB") == 0) { out = EVS_WB;  return true; }
    if (std::strcmp(s, "SWB")== 0) { out = EVS_SWB; return true; }
    if (std::strcmp(s, "FB") == 0) { out = EVS_FB;  return true; }
    return false;
}

static bool parseModeString(const char* s, TelephonyDSP::EraMode& out) {
    if (std::strcmp(s, "g711")      == 0) { out = TelephonyDSP::EraMode::PSTN_G711;     return true; }
    if (std::strcmp(s, "gsm")       == 0) { out = TelephonyDSP::EraMode::GSM_FR;        return true; }
    if (std::strcmp(s, "3g")        == 0) { out = TelephonyDSP::EraMode::AMR_NB_3G;     return true; }
    if (std::strcmp(s, "volte")     == 0) { out = TelephonyDSP::EraMode::AMR_WB_VOLTE;  return true; }
    if (std::strcmp(s, "evs_like")  == 0) { out = TelephonyDSP::EraMode::EVS_LIKE;      return true; }
    if (std::strcmp(s, "evs_native")== 0) { out = TelephonyDSP::EraMode::EVS_NATIVE;    return true; }
    if (std::strcmp(s, "evs_jbm")   == 0) {
#if TELEPHONY_USE_EVS_JBM
        out = TelephonyDSP::EraMode::EVS_JBM;
        return true;
#else
        (void)out;
        return false;
#endif
    }
    if (std::strcmp(s, "opus_voip") == 0) {
#if TELEPHONY_EXPERIMENTAL_NETWORK
        out = TelephonyDSP::EraMode::OPUS_VOIP;
        return true;
#else
        (void)out;
        return false;
#endif
    }
    return false;
}

static void printUsage(const char* prog) {
    std::cout
        << "Usage: " << prog << " <input.wav> [options]\n"
        << "\n"
        << "Options:\n"
        << "  --evs-sr 8000|16000|32000|48000    EVS internal sample rate (default 32000)\n"
        << "  --evs-br 5900|7200|8000|9600|13200|16400|24400|32000|48000|64000|96000|128000\n"
        << "                                      EVS bitrate in bps (default 13200)\n"
        << "  --evs-bw NB|WB|SWB|FB              EVS max bandwidth (default SWB)\n"
        << "  --mode g711|gsm|3g|volte|evs_like|evs_native|evs_jbm|opus_voip\n"
        << "                                      Process only this single mode (default: all)\n"
        << "  --help                              Show this help and exit\n"
        << "\n"
        << "If no --mode is given, all available modes are processed in order.\n"
        << "EVS config flags only affect evs_native and evs_jbm modes.\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 0;
    }

    // Defaults preserve previous behavior (SWB 32 kHz, 13.2 kbps).
    int evsSr = 32000;
    int evsBr = 13200; // EVS_BR_13200
    EVS_Bandwidth evsBw = EVS_SWB;
    bool modeFilterSet = false;
    TelephonyDSP::EraMode modeFilter = TelephonyDSP::EraMode::PSTN_G711;
    std::string inputFile;

    // Simple positional + flag parser. First non-flag argument is the input file.
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];

        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        }
        else if (std::strcmp(a, "--evs-sr") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --evs-sr requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            evsSr = std::atoi(argv[++i]);
            if (!isValidSampleRate(evsSr)) {
                std::cerr << "Error: invalid --evs-sr value: " << evsSr
                          << " (allowed: 8000, 16000, 32000, 48000)\n";
                return 1;
            }
        }
        else if (std::strcmp(a, "--evs-br") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --evs-br requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            evsBr = std::atoi(argv[++i]);
            if (!isValidBitrate(evsBr)) {
                std::cerr << "Error: invalid --evs-br value: " << evsBr << "\n";
                return 1;
            }
        }
        else if (std::strcmp(a, "--evs-bw") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --evs-bw requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            if (!parseBandwidth(argv[++i], evsBw)) {
                std::cerr << "Error: invalid --evs-bw value (allowed: NB, WB, SWB, FB)\n";
                return 1;
            }
        }
        else if (std::strcmp(a, "--mode") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --mode requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            if (!parseModeString(argv[++i], modeFilter)) {
                std::cerr << "Error: invalid --mode value: " << argv[i]
                          << " (or mode not enabled in this build)\n";
                return 1;
            }
            modeFilterSet = true;
        }
        else if (a[0] == '-' && a[1] == '-') {
            std::cerr << "Error: unknown flag: " << a << "\n";
            printUsage(argv[0]);
            return 1;
        }
        else {
            // Positional argument: the input WAV file. Last one wins.
            inputFile = a;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "Error: missing <input.wav>\n";
        printUsage(argv[0]);
        return 1;
    }

    std::cout << "EVS config: " << evsSr << " Hz, " << evsBr << " bps, bw="
              << (evsBw == EVS_NB ? "NB" :
                  evsBw == EVS_WB ? "WB" :
                  evsBw == EVS_SWB ? "SWB" : "FB")
              << std::endl;

    std::vector<TelephonyDSP::EraMode> modes = {
        TelephonyDSP::EraMode::PSTN_G711,
        TelephonyDSP::EraMode::GSM_FR,
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        TelephonyDSP::EraMode::AMR_NB_3G,
        TelephonyDSP::EraMode::AMR_WB_VOLTE,
#endif
        TelephonyDSP::EraMode::EVS_LIKE,
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        TelephonyDSP::EraMode::EVS_NATIVE,  // validated integrated EVS path; see README
#endif
#if TELEPHONY_USE_EVS_JBM && !defined(TELEPHONY_DISTRIBUTION_BUILD)
        // EVS_NATIVE plus the Stage-1 JBM/VoIP receive adapter. Off
        // by default; only emitted when TELEPHONY_USE_EVS_JBM=ON.
        TelephonyDSP::EraMode::EVS_JBM,
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK && !defined(TELEPHONY_DISTRIBUTION_BUILD)
        TelephonyDSP::EraMode::OPUS_VOIP    // experimental, BSD-licensed Opus (non-distribution)
#endif
    };

    for (auto mode : modes) {
        if (modeFilterSet && mode != modeFilter) continue;
        processFile(inputFile, mode, evsSr, evsBr, evsBw);
    }
    return 0;
}
