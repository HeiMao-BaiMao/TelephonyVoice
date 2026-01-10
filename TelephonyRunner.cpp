#define NOMINMAX
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
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
        case TelephonyDSP::EraMode::EVS_LIKE: return "evs";
        case TelephonyDSP::EraMode::Bypass: return "bypass";
        default: return "unknown";
    }
}

void processFile(const std::string& inputFile, TelephonyDSP::EraMode mode) {
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: TelephonyRunner <input.wav>" << std::endl;
        return 0;
    }
    std::string inputFile = argv[1];
    std::vector<TelephonyDSP::EraMode> modes = {
        TelephonyDSP::EraMode::PSTN_G711,
        TelephonyDSP::EraMode::GSM_FR,
        TelephonyDSP::EraMode::AMR_NB_3G,
        TelephonyDSP::EraMode::AMR_WB_VOLTE,
        TelephonyDSP::EraMode::EVS_LIKE
    };
    for (auto mode : modes) processFile(inputFile, mode);
    return 0;
}
