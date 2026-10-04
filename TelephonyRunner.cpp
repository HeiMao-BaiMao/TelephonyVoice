#define NOMINMAX
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

#include "dsp/SignalProcessor.h"

namespace {
namespace fs = std::filesystem;

uint16_t readLe16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8));
}

uint32_t readLe32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void writeLe16(unsigned char* p, uint16_t value) {
    p[0] = static_cast<unsigned char>(value);
    p[1] = static_cast<unsigned char>(value >> 8);
}

void writeLe32(unsigned char* p, uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(value >> (8 * i));
}

// Never reinterpret PCM bytes as native-endian short integers. WAV is little-endian.
float pcm16ToFloat(const unsigned char* p) {
    const int sample = readLe16(p);
    return (sample < 32768 ? sample : sample - 65536) / 32768.0f;
}

int16_t floatToPcm16(float sample) {
    if (!std::isfinite(sample)) throw std::runtime_error("DSP produced a non-finite sample");
    // Symmetric scaling preserves PCM values on a float round trip, including -32768.
    return static_cast<int16_t>(std::lround(std::clamp(sample * 32768.0f, -32768.0f, 32767.0f)));
}

class PcmWavReader {
public:
    explicit PcmWavReader(const fs::path& path) {
        if (!fs::is_regular_file(path)) throw std::runtime_error("Input is not a regular file: " + path.string());
        file.open(path, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot open input: " + path.string());
        const uint64_t fileSize = fs::file_size(path);
        unsigned char header[12];
        readExact(header, sizeof(header));
        if (std::memcmp(header, "RIFF", 4) || std::memcmp(header + 8, "WAVE", 4))
            throw std::runtime_error("Input must be a RIFF/WAVE file");
        const uint64_t riffEnd = uint64_t(readLe32(header + 4)) + 8;
        if (riffEnd < 12 || riffEnd > fileSize) throw std::runtime_error("Truncated or invalid RIFF length");
        bool foundFormat = false, foundData = false;
        uint64_t dataOffset = 0;
        for (uint64_t offset = 12; offset < riffEnd;) {
            if (riffEnd - offset < 8) throw std::runtime_error("Truncated WAV chunk header");
            file.seekg(static_cast<std::streamoff>(offset));
            unsigned char chunk[8];
            readExact(chunk, sizeof(chunk));
            const uint32_t length = readLe32(chunk + 4);
            const uint64_t payload = offset + 8;
            const uint64_t next = payload + length + (length & 1u);
            if (next > riffEnd) throw std::runtime_error("Truncated WAV chunk payload");
            if (!std::memcmp(chunk, "fmt ", 4)) {
                if (foundFormat || length < 16) throw std::runtime_error("Invalid or duplicate WAV fmt chunk");
                unsigned char fmt[40] = {};
                readExact(fmt, std::min<size_t>(length, sizeof(fmt)));
                const uint16_t format = readLe16(fmt);
                if (format == 0xfffe) {
                    static const unsigned char pcmGuid[16] = {
                        1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71
                    };
                    if (length < 40 || readLe16(fmt + 16) < 22 ||
                        uint32_t(readLe16(fmt + 16)) + 18 > length ||
                        readLe16(fmt + 18) != 16 || std::memcmp(fmt + 24, pcmGuid, 16))
                        throw std::runtime_error("Only 16-bit integer PCM WAV is supported");
                } else if (format != 1) {
                    throw std::runtime_error("Only 16-bit integer PCM WAV is supported");
                }
                channels = readLe16(fmt + 2);
                const uint32_t rate = readLe32(fmt + 4);
                if (channels < 1 || channels > 32) throw std::runtime_error("WAV channel count must be 1..32");
                if (rate < 8000 || rate > 192000) throw std::runtime_error("WAV sample rate must be 8000..192000 Hz");
                sampleRate = static_cast<int>(rate);
                if (readLe16(fmt + 14) != 16) throw std::runtime_error("Only 16-bit integer PCM WAV is supported");
                if (readLe16(fmt + 12) != channels * 2 || readLe32(fmt + 8) != rate * channels * 2)
                    throw std::runtime_error("Inconsistent PCM block alignment or byte rate");
                foundFormat = true;
            } else if (!std::memcmp(chunk, "data", 4)) {
                if (foundData) throw std::runtime_error("Multiple WAV data chunks are not supported");
                dataOffset = payload;
                bytesRemaining = length;
                foundData = true;
            }
            offset = next;
        }
        if (!foundFormat || !foundData) throw std::runtime_error("WAV requires fmt and data chunks");
        if (bytesRemaining % (channels * 2)) throw std::runtime_error("PCM data ends in a partial sample frame");
        file.seekg(static_cast<std::streamoff>(dataOffset));
    }

    size_t readFrames(unsigned char* dest, size_t maxFrames) {
        const size_t frames = std::min<uint64_t>(maxFrames, bytesRemaining / (channels * 2));
        const size_t bytes = frames * channels * 2;
        if (bytes) readExact(dest, bytes);
        bytesRemaining -= bytes;
        return frames;
    }

    int channels = 0;
    int sampleRate = 0;

private:
    void readExact(unsigned char* dest, size_t bytes) {
        if (!file.read(reinterpret_cast<char*>(dest), static_cast<std::streamsize>(bytes)))
            throw std::runtime_error("Cannot read WAV data (file may be truncated)");
    }
    std::ifstream file;
    uint64_t bytesRemaining = 0;
};

// Write in a reserved, same-directory temporary folder, then replace the final
// output only after data and the RIFF header have both been successfully closed.
// A failed render never leaves a corrupt WAV or destroys a previous result.
class PcmWavWriter {
public:
    PcmWavWriter(const fs::path& output, int rate, int count)
        : destination(output), sampleRate(rate), channels(count) {
        const auto status = fs::symlink_status(destination);
        if (fs::exists(status) && (!fs::is_regular_file(status) || fs::is_symlink(status)))
            throw std::runtime_error("Output is not a regular file: " + destination.string());
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            temporaryDirectory = destination.parent_path() /
                (".telephonyvoice.tmp." + std::to_string(nonce) + "." + std::to_string(attempt));
            if (fs::create_directory(temporaryDirectory)) break;
            temporaryDirectory.clear();
        }
        if (temporaryDirectory.empty()) throw std::runtime_error("Cannot reserve temporary output file");
        temporaryFile = temporaryDirectory / "audio.wav";
        file.open(temporaryFile, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::error_code ignored;
            fs::remove(temporaryFile, ignored);
            fs::remove(temporaryDirectory, ignored);
            throw std::runtime_error("Cannot open output: " + destination.string());
        }
        const char header[44] = {};
        file.write(header, sizeof(header));
    }

    ~PcmWavWriter() {
        file.close();
        std::error_code ignored;
        if (!temporaryFile.empty()) fs::remove(temporaryFile, ignored);
        if (!temporaryDirectory.empty()) fs::remove(temporaryDirectory, ignored);
    }

    void write(const unsigned char* data, size_t size) {
        if (size > UINT32_MAX - 36u - bytesWritten) throw std::runtime_error("Output exceeds the RIFF/WAV 4 GiB limit");
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!file) throw std::runtime_error("Failed writing WAV output");
        bytesWritten += size;
    }

    void finish() {
        unsigned char header[44] = {};
        std::memcpy(header, "RIFF", 4);
        writeLe32(header + 4, static_cast<uint32_t>(bytesWritten + 36));
        std::memcpy(header + 8, "WAVEfmt ", 8);
        writeLe32(header + 16, 16);
        writeLe16(header + 20, 1);
        writeLe16(header + 22, static_cast<uint16_t>(channels));
        writeLe32(header + 24, static_cast<uint32_t>(sampleRate));
        writeLe32(header + 28, static_cast<uint32_t>(sampleRate * channels * 2));
        writeLe16(header + 32, static_cast<uint16_t>(channels * 2));
        writeLe16(header + 34, 16);
        std::memcpy(header + 36, "data", 4);
        writeLe32(header + 40, static_cast<uint32_t>(bytesWritten));
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        file.flush();
        if (!file) throw std::runtime_error("Failed finalizing WAV header");
        file.close();
        if (file.fail()) throw std::runtime_error("Failed closing WAV output");
#ifdef _WIN32
        if (!MoveFileExW(temporaryFile.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace output: " + destination.string());
#else
        fs::rename(temporaryFile, destination);
#endif
    }

private:
    fs::path destination, temporaryDirectory, temporaryFile;
    std::ofstream file;
    int sampleRate, channels;
    uint64_t bytesWritten = 0;
};

std::string getOutputFilename(const std::string& input, const std::string& suffix) {
    fs::path path(input);
    path.replace_extension("." + suffix + ".wav");
    return path.string();
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
                 int evsSr, int evsBr, EVS_Bandwidth evsBw, int amrNbMode,
                 int amrWbMode, int dtxSidInterval, int g711Law, bool evsScVbr,
                 int opusBw, int opusBr, const TelephonyDSP::AdvancedSettings& advanced = {}) {
    const std::string outputFile = getOutputFilename(inputFile, getModeSuffix(mode));
    std::cout << "Processing " << getModeSuffix(mode) << " -> " << outputFile << "..." << std::endl;
    PcmWavReader wavIn(inputFile);
    const int channels = wavIn.channels;
    const int sampleRate = wavIn.sampleRate;
    std::error_code equivalentError;
    if (fs::equivalent(inputFile, outputFile, equivalentError))
        throw std::runtime_error("Input and output refer to the same file");
    PcmWavWriter wavOut(outputFile, sampleRate, channels);

    TelephonyDSP::SignalProcessor dsp;
    dsp.setSampleRate(sampleRate);
    dsp.setMode(mode);
    if (mode == TelephonyDSP::EraMode::EVS_NATIVE) {
        // EVS config driven by CLI flags (defaults: SWB 32 kHz, 13.2 kbps).
        dsp.setEVSConfig(evsSr, evsBr, evsBw);
    }
    // EVS DTX SID update interval (0 = variable, 3..100 = fixed frames).
    // Pushed for every mode: ChannelProcessor::setEvsDtxSidInterval is a
    // no-op for non-EVS modes, so the worst that happens is the value
    // gets cached and applied next time the user switches to EVS.
    dsp.setEvsDtxSidInterval(dtxSidInterval);
    // Push the AMR-NB mode regardless of the active mode: the value is
    // stashed in every ChannelProcessor and picked up next time the
    // processor is in AMR_NB_3G mode. This way the user can combine
    // --amr-nb-mode with --mode 3g, but it also doesn't hurt to set it
    // for other modes (ChannelProcessor clamps and ignores it).
    dsp.setAmrNbMode(amrNbMode);
    // Same logic for the AMR-WB mode (0..8): cached in every
    // ChannelProcessor and applied the next time the user picks
    // AMR_WB_VOLTE. Safe to set regardless of the active mode.
    dsp.setAmrWbMode(amrWbMode);

#if TELEPHONY_USE_EVS_JBM
    if (mode == TelephonyDSP::EraMode::EVS_JBM) {
        // Same CLI-driven config as EVS_NATIVE.
        dsp.setEVSConfig(evsSr, evsBr, evsBw);
    }
#endif
    dsp.setG711Law(g711Law);
    // EVS Source-Controlled VBR. Cached on every ChannelProcessor and
    // applied the next time EVS_NATIVE / EVS_JBM rebuilds its codec.
    // No-op for non-EVS modes.
    dsp.setEvsScVbrEnabled(evsScVbr);
    // Push the OPUS_BANDWIDTH_* cap regardless of the active mode: the
    // value is cached in every ChannelProcessor and picked up next time
    // the processor is in OPUS_VOIP mode. Safe for non-Opus modes
    // because the value is just stashed.
    dsp.setOpusBandwidth(opusBw);
    // Same for the Opus target bitrate (6000..510000 bps): cached in
    // every ChannelProcessor and applied the next time the user
    // picks OPUS_VOIP. No-op for non-Opus modes.
    dsp.setOpusBitrate(opusBr);
    dsp.setParameters(1.0f, 0.0f, false, 0.0f);
    dsp.setAdvancedSettings(advanced);
    dsp.setSimulateLatency(false); // Disable artificial 100ms latency for runner

    constexpr int blockSize = 1024;
    std::vector<unsigned char> inRaw(blockSize * channels * 2);
    std::vector<unsigned char> outRaw(blockSize * channels * 2);
    std::vector<std::vector<float>> inputs(channels, std::vector<float>(blockSize));
    std::vector<std::vector<float>> outputs(channels, std::vector<float>(blockSize));
    std::vector<float*> inputPtrs(channels), outputPtrs(channels);
    for (int ch = 0; ch < channels; ++ch) {
        inputPtrs[ch] = inputs[ch].data();
        outputPtrs[ch] = outputs[ch].data();
    }
    auto writeOutput = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                writeLe16(outRaw.data() + (i * channels + ch) * 2,
                          static_cast<uint16_t>(floatToPcm16(outputs[ch][i])));
            }
        }
        wavOut.write(outRaw.data(), static_cast<size_t>(frames) * channels * 2);
    };
    uint64_t inputFrames = 0;
    for (;;) {
        const int frames = static_cast<int>(wavIn.readFrames(inRaw.data(), blockSize));
        if (!frames) break;
        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < frames; ++i)
                inputs[ch][i] = pcm16ToFloat(inRaw.data() + (i * channels + ch) * 2);
        }
        dsp.process(inputPtrs.data(), channels, outputPtrs.data(), channels, frames);
        writeOutput(frames);
        inputFrames += frames;
    }

    // Test tones belong to input duration, not the artificial drain period.
    // Changing only this generator control preserves codec/transport history.
    if(advanced.get(TelephonyDSP::AdvancedControl::DtmfDigit)>=0) {
        auto tailSettings=advanced;
        tailSettings.setPlain((size_t)TelephonyDSP::AdvancedControl::DtmfDigit,-1);
        dsp.setAdvancedSettings(tailSettings);
    }
    // Drain codec/resampler state. Durations are measured in samples, not in
    // 1024-sample blocks (which previously meant 64 seconds at 8 kHz).
    // Keep a conservative one-second drain for codec/resampler startup and
    // framing latency, including delayed/very quiet final frames. Stop after
    // 100 ms below the codec noise-floor threshold. No samples are removed from
    // the input duration. Continuous comfort noise is bounded by a 10 s tail.
    for (auto& channel : inputs) std::fill(channel.begin(), channel.end(), 0.0f);
    const int minimumTail = sampleRate;
    const int quietWindow = sampleRate / 10;
    const int maximumTail = sampleRate * 10;
    int flushed = 0, quietSamples = 0;
    while (inputFrames && flushed < maximumTail) {
        const int frames = std::min(blockSize, maximumTail - flushed);
        dsp.process(inputPtrs.data(), channels, outputPtrs.data(), channels, frames);
        for (int i = 0; i < frames; ++i) {
            bool signal = false;
            for (int ch = 0; ch < channels; ++ch)
                signal = signal || std::abs(outputs[ch][i]) > 0.002f;
            quietSamples = signal ? 0 : quietSamples + 1;
        }
        writeOutput(frames);
        flushed += frames;
        if (flushed >= minimumTail && quietSamples >= quietWindow) break;
    }
    if (flushed == maximumTail)
        std::cerr << "Warning: tail reached the 10 second limit for " << getModeSuffix(mode) << "\n";
    wavOut.finish();
}

static bool parseInteger(const char* text, int& value) {
    const char* end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value);
    return result.ec == std::errc{} && result.ptr == end;
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

// String -> OPUS_BANDWIDTH_* constant. The Opus bandwidth values
// (1101..1105) are part of the stable public ABI, so we hard-code them
// here to avoid pulling <opus.h> into the CLI parser. Returns false for
// any unrecognized token.
static bool parseOpusBandwidth(const char* s, int& out) {
    if (std::strcmp(s, "NB")  == 0) { out = 1101; return true; } // OPUS_BANDWIDTH_NARROWBAND
    if (std::strcmp(s, "MB")  == 0) { out = 1102; return true; } // OPUS_BANDWIDTH_MEDIUMBAND
    if (std::strcmp(s, "WB")  == 0) { out = 1103; return true; } // OPUS_BANDWIDTH_WIDEBAND
    if (std::strcmp(s, "SWB") == 0) { out = 1104; return true; } // OPUS_BANDWIDTH_SUPERWIDEBAND
    if (std::strcmp(s, "FB")  == 0) { out = 1105; return true; } // OPUS_BANDWIDTH_FULLBAND
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

static std::vector<TelephonyDSP::EraMode> availableModes() {
    return {
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
}

static void printUsage(const char* prog) {
    std::cout
        << "Usage: " << prog << " <input.wav> [options]\n"
        << "\n"
        << "Options:\n"
        << "  --evs-sr 8000|16000|32000|48000    EVS internal sample rate (default 32000)\n"
        << "  --evs-br 5900|7200|8000|9600|13200|16400|24400|32000|48000|64000|96000|128000\n"
        << "                                      EVS bitrate in bps (default 13200); 5900 selects SC-VBR.\n"
        << "  --evs-bw NB|WB|SWB|FB              EVS max bandwidth (default SWB)\n"
        << "  --evs-dtx-sid-interval N            EVS DTX SID update interval in 20 ms frames\n"
        << "                                      (0 = variable [default], 3..100 = fixed).\n"
        << "                                      Only affects evs_native and evs_jbm.\n"
        << "  --evs-sc-vbr                        Select EVS 5.9 kbps average SC-VBR, NB/WB (default off).\n"
        << "                                      Only affects evs_native and evs_jbm.\n"
        << "  --mode NAME                         Process one available mode (default: all)\n"
        << "  --g711-law ulaw|alaw                G.711 companding law for the g711 mode\n"
        << "                                      (default: ulaw). Only affects the g711 path.\n"
        << "  --amr-nb-mode N                     AMR-NB bitrate mode 0..7 (default 7 = 12.2 kbps).\n"
        << "                                      Only affects the 3g path.\n"
        << "  --amr-wb-mode N                     AMR-WB bitrate mode 0..8 (default 2 = 12.65 kbps).\n"
        << "                                      Only affects the volte path.\n"
        << "  --opus-bw NB|MB|WB|SWB|FB           Opus max bandwidth cap (default FB)\n"
        << "                                      (NB=4kHz, MB=6kHz, WB=8kHz, SWB=12kHz, FB=20kHz).\n"
        << "                                      Only affects the opus_voip path.\n"
        << "  --opus-bitrate BPS                  Opus target bitrate in bps (6000..510000,\n"
        << "                                      default 24000). Only affects opus_voip.\n"
        << "  --help                              Show this help and exit\n"
        << "\n"
        << "If no --mode is given, all available modes are processed in order.\n"
        << "EVS config flags only affect evs_native and evs_jbm modes.\n"
        << "EVS NB/8000 Hz supports at most 24400 bps; bandwidth is limited to the rate.\n"
        << "Input: 16-bit integer PCM RIFF/WAVE, 1..32 channels, 8000..192000 Hz.\n"
        << "Outputs: <input-stem>.<mode>.wav beside the input; successful runs replace them.\n"
        << "Nonempty inputs get a codec/resampler tail (at least 1 s, at most 10 s).\n"
        << "Use -- before an input filename beginning with a hyphen.\n"
        << "Exit status: 0 for success/help, 1 for invalid arguments or processing failure.\n"
        << "Available modes:";
    std::cout << "\nAdvanced simulation (numeric values; disabled by default):\n";
    for(const auto& d:TelephonyDSP::advancedDescriptors)
        std::cout << "  --" << d.key << " VALUE  " << d.title << " [" << d.minimum << ".." << d.maximum << ", default " << d.initial << "] " << d.units << "\n";
    std::cout << "  Jitter: 0 uniform, 1 gamma, 2 Weibull, 3 Pareto.\n"
                 "  Opus duration: 0=2.5, 1=5, 2=10, 3=20, 4=40, 5=60 ms.\n"
                 "  DTMF: -1 off, 0..15 selects 0123456789*#ABCD.\n"
                 "  VAD: 0 energy+hangover, 1 reference VAD2 core (non-distribution).\n"
                 "  Available modes:";
    for (const auto mode : availableModes()) std::cout << " " << getModeSuffix(mode);
    std::cout << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // Defaults preserve previous behavior (SWB 32 kHz, 13.2 kbps).
    int evsSr = 32000;
    int evsBr = 13200; // EVS_BR_13200
    EVS_Bandwidth evsBw = EVS_SWB;
    int g711Law = 0; // 0 = mu-law, 1 = A-law
    // Opus OPUS_BANDWIDTH_* cap; defaults to FB (20 kHz) which matches
    // Opus's own default. The values are the public Opus bandwidth
    // constants (1101..1105).
    int opusBw = 1105; // OPUS_BANDWIDTH_FULLBAND
    int opusBr = 24000; // Opus target bitrate in bps; matches Opus's own default.
    int amrNbMode = 7;  // AMR-NB 12.2 kbps (MR122); matches AMRNBCodec's default.
    int amrWbMode = 2;  // AMR-WB 12.65 kbps; matches AMRWBCodec's default.
    int dtxSidInterval = 0; // 0 = variable SID (codec default).
    bool evsScVbr = false;  // EVS Source-Controlled VBR is opt-in.
    bool modeFilterSet = false;
    TelephonyDSP::EraMode modeFilter = TelephonyDSP::EraMode::PSTN_G711;
    std::string inputFile;
    TelephonyDSP::AdvancedSettings advanced;

    bool positionalOnly = false;
    bool inputProvided = false;
    // Exactly one input path; -- allows a filename beginning with a hyphen.
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];

        if (positionalOnly) {
            if (inputProvided) {
                std::cerr << "Error: expected exactly one input WAV file\n";
                return 1;
            }
            inputFile = a;
            inputProvided = true;
            continue;
        }
        if (std::strcmp(a, "--") == 0) {
            positionalOnly = true;
            continue;
        }
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
            const bool parsed = parseInteger(argv[++i], evsSr);
            if (!parsed || !isValidSampleRate(evsSr)) {
                std::cerr << "Error: invalid --evs-sr value: " << argv[i]
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
            const bool parsed = parseInteger(argv[++i], evsBr);
            if (!parsed || !isValidBitrate(evsBr)) {
                std::cerr << "Error: invalid --evs-br value: " << argv[i] << "\n";
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
        else if (std::strcmp(a, "--evs-dtx-sid-interval") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --evs-dtx-sid-interval requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            const bool parsed = parseInteger(argv[++i], dtxSidInterval);
            // 0 = variable SID; 3..100 = fixed frames. The codec
            // implementation also accepts 1..2 but normalizes them to
            // 0 (no DTX) -- reject them here so the CLI is explicit
            // about that behavior.
            if (!parsed || dtxSidInterval < 0 || dtxSidInterval > 100
                || (dtxSidInterval >= 1 && dtxSidInterval <= 2)) {
                std::cerr << "Error: invalid --evs-dtx-sid-interval value: "
                          << argv[i]
                          << " (allowed: 0 or 3..100; 1..2 is rejected)\n";
                return 1;
            }
        }
        else if (std::strcmp(a, "--evs-sc-vbr") == 0) {
            evsScVbr = true;
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
        else if (std::strcmp(a, "--g711-law") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --g711-law requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            const char* lawArg = argv[++i];
            if (std::strcmp(lawArg, "ulaw") == 0 || std::strcmp(lawArg, "mu-law") == 0 || std::strcmp(lawArg, "mulaw") == 0) {
                g711Law = 0;
            } else if (std::strcmp(lawArg, "alaw") == 0 || std::strcmp(lawArg, "A-law") == 0) {
                g711Law = 1;
            } else {
                std::cerr << "Error: invalid --g711-law value: " << lawArg
                          << " (allowed: ulaw, alaw)\n";
                return 1;
            }
        }
        else if (std::strcmp(a, "--amr-nb-mode") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --amr-nb-mode requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            const bool parsed = parseInteger(argv[++i], amrNbMode);
            if (!parsed || amrNbMode < 0 || amrNbMode > 7) {
                std::cerr << "Error: invalid --amr-nb-mode value: " << argv[i]
                          << " (allowed: 0..7)\n";
                return 1;
            }
        }
        else if (std::strcmp(a, "--amr-wb-mode") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --amr-wb-mode requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            const bool parsed = parseInteger(argv[++i], amrWbMode);
            if (!parsed || amrWbMode < 0 || amrWbMode > 8) {
                std::cerr << "Error: invalid --amr-wb-mode value: " << argv[i]
                          << " (allowed: 0..8)\n";
                return 1;
            }
        }
        else if (std::strcmp(a, "--opus-bw") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --opus-bw requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            if (!parseOpusBandwidth(argv[++i], opusBw)) {
                std::cerr << "Error: invalid --opus-bw value (allowed: NB, MB, WB, SWB, FB)\n";
                return 1;
            }
        }
        else if (std::strcmp(a, "--opus-bitrate") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "Error: --opus-bitrate requires a value\n";
                printUsage(argv[0]);
                return 1;
            }
            const bool parsed = parseInteger(argv[++i], opusBr);
            if (!parsed || opusBr < 6000 || opusBr > 510000) {
                std::cerr << "Error: invalid --opus-bitrate value: " << argv[i]
                          << " (allowed: 6000..510000)\n";
                return 1;
            }
        }
        else if (std::strncmp(a, "--", 2) == 0) {
            bool matched = false;
            for (size_t index=0; index<TelephonyDSP::advancedDescriptors.size(); ++index) {
                const auto& descriptor=TelephonyDSP::advancedDescriptors[index];
                if(std::string_view(a+2)!=descriptor.key) continue;
                matched=true;
                if(i+1>=argc) { std::cerr << "Error: " << a << " requires a value\n"; return 1; }
                const char* valueText=argv[++i];
                double value=0;
                const auto parsed=std::from_chars(valueText,valueText+std::strlen(valueText),value);
                if(parsed.ec!=std::errc{} || parsed.ptr!=valueText+std::strlen(valueText) || !advanced.setPlain(index,value)) {
                    std::cerr << "Error: invalid " << a << " (allowed " << descriptor.minimum << ".." << descriptor.maximum << ")\n";
                    return 1;
                }
                break;
            }
            if(!matched) { std::cerr << "Error: unknown flag: " << a << "\n"; return 1; }
        }
        else if (a[0] == '-') {
            std::cerr << "Error: unknown flag: " << a << "\n";
            printUsage(argv[0]);
            return 1;
        }
        else {
            if (inputProvided) {
                std::cerr << "Error: expected exactly one input WAV file\n";
                return 1;
            }
            inputFile = a;
            inputProvided = true;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "Error: missing <input.wav>\n";
        printUsage(argv[0]);
        return 1;
    }

    using A=TelephonyDSP::AdvancedControl;
#ifdef TELEPHONY_DISTRIBUTION_BUILD
    if(advanced.get(A::VadMode)==1 || advanced.enabled(A::EvsAmrWbIo) || advanced.enabled(A::EvsAutoBandwidth)) {
        std::cerr << "Error: reference VAD2 and EVS controls are unavailable in distribution builds\n"; return 1;
    }
#endif
#if !TELEPHONY_EXPERIMENTAL_NETWORK
    if(advanced.get(A::OpusForceMode)!=0 || advanced.enabled(A::OpusFecOnly)) {
        std::cerr << "Error: Opus is unavailable in this build\n"; return 1;
    }
#endif
    if(advanced.enabled(A::HostClock)) { std::cerr << "Error: host-clock requires a VST host; CLI renders use deterministic sample time\n"; return 1; }
    if(advanced.enabled(A::EvsAmrWbIo) && (evsSr<16000 || evsScVbr)) {
        std::cerr << "Error: EVS AMR-WB IO requires 16000/32000/48000 Hz and SC-VBR off\n"; return 1;
    }
#if TELEPHONY_USE_EVS_JBM
    if(advanced.enabled(A::EvsAmrWbIo) && (!modeFilterSet || modeFilter==TelephonyDSP::EraMode::EVS_JBM)) {
        std::cerr << "Error: EVS AMR-WB IO is not supported by JBM; select --mode evs_native\n"; return 1;
    }
#endif
    const int forcedMode=(int)advanced.get(A::OpusForceMode);
    if((forcedMode==1 || forcedMode==2) && advanced.get(A::OpusFrameDuration)<2) {
        std::cerr << "Error: forced SILK/Hybrid requires at least 10 ms Opus frames\n"; return 1;
    }
    if(forcedMode==2 && (opusBw<1104 || opusBr<16000)) {
        std::cerr << "Error: forced Hybrid requires SWB/FB and at least 16000 bps\n"; return 1;
    }
    if(advanced.enabled(A::OpusFecOnly) && (advanced.get(A::OpusFrameDuration)<2 || forcedMode==3 || !advanced.enabled(A::OpusFecEnabled) || advanced.get(A::OpusFecPercent)==0 || !advanced.enabled(A::NetworkEnabled))) {
        std::cerr << "Error: FEC-only requires network-enabled, FEC enabled, expected loss >0, >=10ms and SILK/Hybrid-capable mode\n"; return 1;
    }
    if(advanced.enabled(A::Redundancy) && (!advanced.enabled(A::PacketFormat) || !advanced.enabled(A::NetworkEnabled))) {
        std::cerr << "Error: RFC2198 requires --packet-format 1 --network-enabled 1\n"; return 1;
    }
    const auto modes = availableModes();

    if (modeFilterSet && std::find(modes.begin(), modes.end(), modeFilter) == modes.end()) {
        std::cerr << "Error: requested mode is not enabled in this build\n";
        return 1;
    }
    const bool renderEvs = std::any_of(modes.begin(), modes.end(), [&](TelephonyDSP::EraMode mode) {
        if (modeFilterSet && mode != modeFilter) return false;
        return mode == TelephonyDSP::EraMode::EVS_NATIVE
#if TELEPHONY_USE_EVS_JBM
            || mode == TelephonyDSP::EraMode::EVS_JBM
#endif
            ;
    });
    if (renderEvs) {
        EVS_EncOptions options;
        evs_enc_options_init(&options);
        options.dtx_enable = 1;
        options.dtx_sid_interval = dtxSidInterval;
        options.sc_vbr_enable = evsScVbr ? 1 : 0;
        // Share the same guard as the C API. Individually valid flag values can
        // form unsupported combinations (notably NB above 24.4 kbps).
        if (evs_enc_normalize_config(evsSr, &evsBr, &evsBw, &options) != EVS_OK) {
            std::cerr << "Error: unsupported EVS configuration; NB/8000 Hz requires at most 24400 bps\n";
            return 1;
        }
        evsScVbr = options.sc_vbr_enable != 0;
        dtxSidInterval = options.dtx_sid_interval;
        std::cout << "EVS config: " << evsSr << " Hz, " << evsBr << " bps, bw="
                  << (evsBw == EVS_NB ? "NB" : evsBw == EVS_WB ? "WB" :
                      evsBw == EVS_SWB ? "SWB" : "FB")
                  << (evsScVbr ? ", SC-VBR" : "") << std::endl;
    }
    try {
        for (auto mode : modes) {
            if (modeFilterSet && mode != modeFilter) continue;
            processFile(inputFile, mode, evsSr, evsBr, evsBw, amrNbMode, amrWbMode,
                        dtxSidInterval, g711Law, evsScVbr, opusBw, opusBr, advanced);
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
