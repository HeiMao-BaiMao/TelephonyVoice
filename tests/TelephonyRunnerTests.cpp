// Synthetic, offline regression coverage of the real CLI parser and WAV path.
// Compiling the runner here also permits direct checks of endian conversion and
// failure cleanup without exposing implementation helpers as a public API.
#define main telephonyRunnerMain
#include "../TelephonyRunner.cpp"
#undef main

#include <array>
#include <functional>
#include <limits>

namespace {
int failures = 0;

void check(bool ok, const std::string& description) {
    std::cout << (ok ? "ok: " : "FAIL: ") << description << '\n';
    if (!ok) ++failures;
}

int run(std::vector<std::string> args) {
    args.insert(args.begin(), "TelephonyRunner");
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    return telephonyRunnerMain(static_cast<int>(argv.size()), argv.data());
}

std::vector<unsigned char> makeWav(int rate = 48000, int channels = 1, int frames = 1703) {
    std::vector<unsigned char> bytes(44 + frames * channels * 2);
    std::memcpy(bytes.data(), "RIFF", 4);
    writeLe32(bytes.data() + 4, static_cast<uint32_t>(bytes.size() - 8));
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    writeLe32(bytes.data() + 16, 16);
    writeLe16(bytes.data() + 20, 1);
    writeLe16(bytes.data() + 22, static_cast<uint16_t>(channels));
    writeLe32(bytes.data() + 24, rate);
    writeLe32(bytes.data() + 28, rate * channels * 2);
    writeLe16(bytes.data() + 32, static_cast<uint16_t>(channels * 2));
    writeLe16(bytes.data() + 34, 16);
    std::memcpy(bytes.data() + 36, "data", 4);
    writeLe32(bytes.data() + 40, frames * channels * 2);
    // Put the only signal in the final partial input block. A dropped or
    // zero-padded short read would lose it entirely. Right stays silent to
    // detect interleaving mistakes.
    for (int i = std::min(1024, frames); i < frames; ++i) {
        const int sample = static_cast<int>(10000 * std::sin(i * 2 * 3.141592653589793 * 997 / rate));
        writeLe16(bytes.data() + 44 + i * channels * 2, static_cast<uint16_t>(sample));
    }
    return bytes;
}

void save(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("Cannot write test fixture");
}

std::vector<unsigned char> load(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct ScratchDirectory {
    fs::path path;
    ScratchDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int i = 0; i < 100; ++i) {
            path = fs::temp_directory_path() / ("telephony-cli-" + std::to_string(nonce) + "-" + std::to_string(i));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("Cannot reserve test directory");
    }
    ~ScratchDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void testParsing(const fs::path& input) {
    check(run({"--help"}) == 0, "help succeeds without an input");
    check(run({}) != 0, "missing arguments fail");
    check(run({input.string(), input.string()}) != 0, "multiple input paths fail");
    check(run({"", input.string()}) != 0, "empty extra positional argument is rejected");
    check(run({input.string(), "--unknown"}) != 0, "unknown long option fails");
    check(run({input.string(), "-x"}) != 0, "unknown short option fails");
    const std::array<const char*, 6> numericFlags = {{
        "--evs-sr", "--evs-br", "--evs-dtx-sid-interval", "--amr-nb-mode", "--amr-wb-mode", "--opus-bitrate"
    }};
    for (const auto* flag : numericFlags) {
        for (const auto* value : {"", "abc", "0oops", "32000x", "99999999999999999999999999999999", "-99999999999999999999999999999"})
            check(run({input.string(), "--mode", "g711", flag, value}) != 0,
                  std::string("strict integer parsing: ") + flag + " " + value);
        check(run({input.string(), flag}) != 0, std::string("missing value: ") + flag);
    }
    for (const auto& value : std::vector<std::pair<std::string, std::string>>{
             {"--evs-sr", "44100"}, {"--evs-br", "12345"},
             {"--evs-dtx-sid-interval", "1"}, {"--evs-dtx-sid-interval", "2"},
             {"--evs-dtx-sid-interval", "101"}, {"--amr-nb-mode", "-1"},
             {"--amr-nb-mode", "8"}, {"--amr-wb-mode", "9"},
             {"--opus-bitrate", "5"}, {"--opus-bitrate", "6"},
             {"--opus-bitrate", "5999"}, {"--opus-bitrate", "510001"}})
        check(run({input.string(), "--mode", "g711", value.first, value.second}) != 0,
              "out-of-range value fails: " + value.first + " " + value.second);
    check(run({input.string(), "--mode", "missing"}) != 0, "unknown mode fails");
#ifdef TELEPHONY_DISTRIBUTION_BUILD
    for (const auto* mode : {"3g", "volte", "evs_native", "evs_jbm", "opus_voip"})
        check(run({input.string(), "--mode", mode}) != 0, std::string("unavailable mode fails: ") + mode);
#endif
#if !TELEPHONY_EXPERIMENTAL_NETWORK
    check(run({input.string(), "--mode", "opus_voip"}) != 0, "disabled Opus fails");
#endif
#if !TELEPHONY_USE_EVS_JBM
    check(run({input.string(), "--mode", "evs_jbm"}) != 0, "disabled JBM fails");
#endif
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    for (const auto& config : std::vector<std::vector<std::string>>{
             {"--evs-sr", "8000", "--evs-br", "32000"},
             {"--evs-sr", "8000", "--evs-br", "128000", "--evs-bw", "FB"},
             {"--evs-sr", "16000", "--evs-br", "128000", "--evs-bw", "NB"}}) {
        auto args = std::vector<std::string>{input.string(), "--mode", "evs_native"};
        args.insert(args.end(), config.begin(), config.end());
        check(run(args) != 0, "unsafe EVS narrowband/high-bitrate combination is rejected before rendering");
    }
    check(!fs::exists(getOutputFilename(input.string(), "evs_native")), "invalid EVS combinations create no output");
#endif
    check(!fs::exists(getOutputFilename(input.string(), "g711")), "invalid arguments create no output");
}

void testInvalidWavs(const fs::path& root) {
    const fs::path input = root / "invalid.wav";
    const fs::path output = root / "invalid.g711.wav";
    const auto valid = makeWav();
    const std::vector<unsigned char> existing = {'p', 'r', 'e', 'v', 'i', 'o', 'u', 's'};
    save(output, existing);
    const std::vector<std::pair<std::string, std::function<void(std::vector<unsigned char>&)>>> cases = {
        {"not RIFF", [](auto& b) { b[0] = 'X'; }},
        {"not WAVE", [](auto& b) { b[8] = 'X'; }},
        {"float format", [](auto& b) { writeLe16(b.data() + 20, 3); }},
        {"zero channels", [](auto& b) { writeLe16(b.data() + 22, 0); }},
        {"excessive channels", [](auto& b) { writeLe16(b.data() + 22, 65535); }},
        {"zero sample rate", [](auto& b) { writeLe32(b.data() + 24, 0); }},
        {"excessive sample rate", [](auto& b) { writeLe32(b.data() + 24, UINT32_MAX); }},
        {"wrong bit depth", [](auto& b) { writeLe16(b.data() + 34, 24); }},
        {"bad alignment", [](auto& b) { writeLe16(b.data() + 32, 4); }},
        {"bad byte rate", [](auto& b) { writeLe32(b.data() + 28, 1); }},
        {"truncated payload", [](auto& b) { b.pop_back(); }},
        {"oversized RIFF", [](auto& b) { writeLe32(b.data() + 4, UINT32_MAX); }},
        {"oversized data", [](auto& b) { writeLe32(b.data() + 40, UINT32_MAX); }},
        {"partial sample", [](auto& b) { writeLe32(b.data() + 40, static_cast<uint32_t>(b.size() - 45)); }},
        {"missing format", [](auto& b) { std::memcpy(b.data() + 12, "JUNK", 4); }},
        {"missing data", [](auto& b) { std::memcpy(b.data() + 36, "JUNK", 4); }},
        {"duplicate fmt", [](auto& b) {
            const std::vector<unsigned char> fmt(b.begin() + 12, b.begin() + 36);
            b.insert(b.end(), fmt.begin(), fmt.end());
            writeLe32(b.data() + 4, static_cast<uint32_t>(b.size() - 8));
        }},
        {"duplicate data", [](auto& b) {
            const std::array<unsigned char, 8> chunk = {{'d', 'a', 't', 'a', 0, 0, 0, 0}};
            b.insert(b.end(), chunk.begin(), chunk.end());
            writeLe32(b.data() + 4, static_cast<uint32_t>(b.size() - 8));
        }},
        {"incomplete header", [](auto& b) { b.resize(10); }}
    };
    for (const auto& test : cases) {
        auto bytes = valid;
        test.second(bytes);
        save(input, bytes);
        check(run({input.string(), "--mode", "g711"}) != 0, "invalid WAV fails: " + test.first);
        check(load(output) == existing, "failed render preserves existing result: " + test.first);
    }
    check(run({(root / "missing.wav").string(), "--mode", "g711"}) != 0, "missing input fails");
    check(run({root.string(), "--mode", "g711"}) != 0, "directory input fails");
}

void testPcmConversion() {
    bool identical = true;
    unsigned char bytes[2];
    for (int sample = -32768; sample <= 32767; ++sample) {
        writeLe16(bytes, static_cast<uint16_t>(sample));
        if (floatToPcm16(pcm16ToFloat(bytes)) != sample) identical = false;
    }
    check(identical, "all 65536 PCM16 values survive the float round trip");
    check(floatToPcm16(1.0f) == 32767 && floatToPcm16(-1.0f) == -32768, "full-scale endpoints clip safely");
    check(floatToPcm16(2.0f) == 32767 && floatToPcm16(-2.0f) == -32768, "over-range samples clip safely");
    for (const float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        bool rejected = false;
        try { (void)floatToPcm16(value); } catch (const std::exception&) { rejected = true; }
        check(rejected, "non-finite DSP output is rejected");
    }
}

void inspectRender(const fs::path& input, int rate, int channels, int inputFrames) {
    check(run({input.string(), "--mode", "g711"}) == 0, "render succeeds: " + input.filename().string());
    const auto bytes = load(getOutputFilename(input.string(), "g711"));
    check(bytes.size() >= 44, "output has a WAV header");
    if (bytes.size() < 44) return;
    check(readLe32(bytes.data() + 4) + uint64_t(8) == bytes.size(), "RIFF output length is exact");
    check(readLe32(bytes.data() + 40) + uint64_t(44) == bytes.size(), "PCM output length is exact");
    check(readLe16(bytes.data() + 22) == channels && readLe32(bytes.data() + 24) == static_cast<uint32_t>(rate), "rate and channels are preserved");
    const uint64_t outputFrames = (bytes.size() - 44) / (channels * 2);
    check(outputFrames >= static_cast<uint64_t>(inputFrames + rate), "tail drains at least one second for resampler startup");
    // G.711 has no persistent CNG at unity gain. Even at 192 kHz silence should
    // drain in one second plus at most one processing block, rather than timing out or chopping the input.
    check(outputFrames <= static_cast<uint64_t>(inputFrames + rate + 1024), "quiet tail drains within a second plus one block at every rate");
    bool leftSignal = false, rightSignal = false;
    for (size_t i = 44; i < bytes.size(); i += channels * 2) {
        if (std::abs(pcm16ToFloat(bytes.data() + i)) > 0.005f) leftSignal = true;
        if (channels > 1 && std::abs(pcm16ToFloat(bytes.data() + i + 2)) > 0.001f) rightSignal = true;
    }
    check(leftSignal, "short fixture produces non-silent output including final partial block");
    if (channels > 1) check(!rightSignal, "silent stereo channel remains isolated");
    const auto firstRun = bytes;
    check(run({input.string(), "--mode", "g711"}) == 0, "existing output can be replaced");
    check(load(getOutputFilename(input.string(), "g711")) == firstRun, "repeated conversion is deterministic");
}

void testFiles(const fs::path& root) {
    check(fs::path(getOutputFilename((root / "dir.with.dots" / "input").string(), "g711")) == root / "dir.with.dots" / "input.g711.wav", "dotted directory does not become the input extension");
    check(fs::path(getOutputFilename((root / ".hidden").string(), "g711")) == root / ".hidden.g711.wav", "extensionless hidden filename is preserved");
    for (int rate : {8000, 44100, 48000, 192000}) {
        const auto input = root / ("signal " + std::to_string(rate) + ".wav");
        save(input, makeWav(rate, 2));
        inspectRender(input, rate, 2, 1703);
    }
    auto padded = makeWav();
    const std::array<unsigned char, 10> junk = {{'J', 'U', 'N', 'K', 1, 0, 0, 0, 'x', 0}};
    padded.insert(padded.begin() + 12, junk.begin(), junk.end());
    writeLe32(padded.data() + 4, static_cast<uint32_t>(padded.size() - 8));
    const auto extraChunk = root / "padded.wav";
    save(extraChunk, padded);
    inspectRender(extraChunk, 48000, 1, 1703);

    auto extensible = makeWav();
    extensible.insert(extensible.begin() + 36, 24, 0);
    writeLe32(extensible.data() + 4, static_cast<uint32_t>(extensible.size() - 8));
    writeLe32(extensible.data() + 16, 40);
    writeLe16(extensible.data() + 20, 0xfffe);
    writeLe16(extensible.data() + 36, 22);
    writeLe16(extensible.data() + 38, 16);
    const unsigned char pcmGuid[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
    std::memcpy(extensible.data() + 44, pcmGuid, 16);
    const auto extendedInput = root / "extensible.wav";
    save(extendedInput, extensible);
    inspectRender(extendedInput, 48000, 1, 1703);

#ifndef TELEPHONY_DISTRIBUTION_BUILD
    const auto scVbr = root / "sc-vbr.wav";
    save(scVbr, makeWav());
    check(run({scVbr.string(), "--mode", "evs_native", "--evs-br", "5900"}) == 0,
          "5900 bps automatically selects the supported SC-VBR configuration");
    check(run({scVbr.string(), "--mode", "evs_native", "--evs-sc-vbr"}) == 0,
          "SC-VBR flag normalizes the default bitrate to a supported configuration");
#endif

    for (const auto* bitrate : {"6000", "510000"}) {
        const auto opusInput = root / (std::string("opus-") + bitrate + ".wav");
        save(opusInput, makeWav());
#if TELEPHONY_EXPERIMENTAL_NETWORK && !defined(TELEPHONY_DISTRIBUTION_BUILD)
        const char* mode = "opus_voip";
#else
        const char* mode = "g711"; // Exercise the shared parser without unavailable codecs.
#endif
        check(run({opusInput.string(), "--mode", mode, "--opus-bitrate", bitrate}) == 0,
              std::string("Opus bitrate boundary is accepted: ") + bitrate + " bps");
    }

    const auto empty = root / "empty.wav";
    save(empty, makeWav(48000, 1, 0));
    check(run({empty.string(), "--mode", "g711"}) == 0, "empty PCM file succeeds");
    check(fs::file_size(root / "empty.g711.wav") == 44, "empty input has no fabricated tail");

    const auto blocked = root / "blocked.wav";
    save(blocked, makeWav());
    fs::create_directory(root / "blocked.g711.wav");
    check(run({blocked.string(), "--mode", "g711"}) != 0, "unwritable output target fails");
    check(fs::is_directory(root / "blocked.g711.wav"), "output target directory is preserved");

    const auto linked = root / "linked.wav";
    save(linked, makeWav());
    std::error_code linkError;
    fs::create_hard_link(linked, root / "linked.g711.wav", linkError);
    if (!linkError) {
        const auto original = load(linked);
        check(run({linked.string(), "--mode", "g711"}) != 0, "input/output alias is rejected");
        check(load(linked) == original, "input/output alias does not corrupt input");
    }
    // Temporarily use the scratch directory so the actual argv starts with '-'.
    const auto previous = fs::current_path();
    fs::current_path(root);
    save("-input.wav", makeWav());
    check(run({"--mode", "g711", "--", "-input.wav"}) == 0, "-- permits a leading-hyphen input path");
    fs::current_path(previous);

    const auto protectedOutput = root / "preserved.wav";
    const std::vector<unsigned char> existing = {'o', 'l', 'd'};
    save(protectedOutput, existing);
    {
        PcmWavWriter incomplete(protectedOutput, 48000, 1);
        const unsigned char sample[2] = {};
        incomplete.write(sample, 2);
        // Destruction without finish models a DSP or read failure.
    }
    check(load(protectedOutput) == existing, "aborted write preserves previous output");
    const auto blockedCommit = root / "blocked-commit.wav";
    bool commitFailed = false;
    try {
        PcmWavWriter incomplete(blockedCommit, 48000, 1);
        fs::create_directory(blockedCommit);
        incomplete.finish();
    } catch (const std::exception&) { commitFailed = true; }
    check(commitFailed && fs::is_directory(blockedCommit), "failed final replacement is reported and preserves destination");
    bool noTemporaries = true;
    for (const auto& entry : fs::directory_iterator(root))
        if (entry.path().filename().string().find(".tmp.") != std::string::npos) noTemporaries = false;
    check(noTemporaries, "success and failure remove temporary outputs");
}
} // namespace

int main() {
    try {
        ScratchDirectory scratch;
        const auto input = scratch.path / "input.wav";
        save(input, makeWav());
        testPcmConversion();
        testParsing(input);
        testInvalidWavs(scratch.path);
        testFiles(scratch.path);
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    std::cout << "CLI failures: " << failures << '\n';
    return failures ? 1 : 0;
}
