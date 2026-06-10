#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <memory>
#include <atomic>
#include <algorithm>

// External Libraries
#include "CDSPResampler.h"
#include "evs_api.h"
#if TELEPHONY_USE_EVS_JBM
// Stage-1 JBM/VoIP receive adapter (experimental, float EVS only).
// Only the public C API is pulled in here; the rest of the JBM is sealed
// inside evs_api_rx.c / external/3gpp-evs.
#include "evs_api_rx.h"
#endif

extern "C" {
    unsigned char linear2ulaw(int pcm_val);
    int ulaw2linear(unsigned char u_val);
    unsigned char linear2alaw(int pcm_val);
    int alaw2linear(unsigned char a_val);
}

namespace TelephonyDSP {

enum class EraMode {
        PSTN_G711 = 0,
        GSM_FR,
        AMR_NB_3G,
        AMR_WB_VOLTE,
        EVS_LIKE,    // Filter-only stand-in (safe for distribution, no codec)
        EVS_NATIVE,  // Real 3GPP EVS reference encoder+decoder (personal use)
        Bypass,
        // Experimental additions (BSD-licensed; non-distribution only).
        // Inserted at the end so existing numeric values remain stable.
        OPUS_VOIP,   // Opus encoder/decoder at VOIP-tuned settings (non-distribution)
#if TELEPHONY_USE_EVS_JBM
        // EVS_NATIVE plus the Stage-1 JBM/VoIP receive adapter
        // (evs_api_rx / 3GPP EvsRXlib). Off by default; enabled when
        // TELEPHONY_USE_EVS_JBM=ON. Non-distribution, float EVS only.
        EVS_JBM
#endif
    };

    enum class RouteEndpoint {
        FixedLine = 0,
        Mobile2G,
        Mobile3G,
        Mobile4G,
        Mobile5G,
        Mobile5GNative,
#if TELEPHONY_USE_EVS_JBM
        Mobile5GJbm
#endif
    };

    enum class DegradationSegment {
        Both = 0,
        InputToExchange,
        ExchangeToOutput,
        None
    };

    static const int LATENCY_MS = 100;

    // Simple Biquad Filter (Direct Form I)
    struct Biquad {
        float b0, b1, b2, a1, a2;
        float x1, x2, y1, y2;

        Biquad() { reset(); }
        void reset() {
            b0 = b1 = b2 = a1 = a2 = 0.0f;
            x1 = x2 = y1 = y2 = 0.0f;
        }
        
        // Calculate coefficients for Butterworth Highpass
        void setHighpass(float freq, float sampleRate) {
            float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
            float cosw0 = std::cos(w0);
            float alpha = std::sin(w0) / std::sqrt(2.0f);

            float norm = 1.0f / (1.0f + alpha);
            b0 = ((1.0f + cosw0) / 2.0f) * norm;
            b1 = -(1.0f + cosw0) * norm;
            b2 = ((1.0f + cosw0) / 2.0f) * norm;
            a1 = (-2.0f * cosw0) * norm;
            a2 = (1.0f - alpha) * norm;
        }

        // Calculate coefficients for Butterworth Lowpass
        void setLowpass(float freq, float sampleRate) {
            float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
            float cosw0 = std::cos(w0);
            float alpha = std::sin(w0) / std::sqrt(2.0f);

            float norm = 1.0f / (1.0f + alpha);
            b0 = ((1.0f - cosw0) / 2.0f) * norm;
            b1 = (1.0f - cosw0) * norm;
            b2 = ((1.0f - cosw0) / 2.0f) * norm;
            a1 = (-2.0f * cosw0) * norm;
            a2 = (1.0f - alpha) * norm;
        }

        inline float process(float in) {
            float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in;
            y2 = y1; y1 = out;
            return out;
        }
    };

    class RingBuffer {
    public:
        RingBuffer(size_t size = 0);
        void resize(size_t size);
        void reset();
        size_t getReadAvailable() const;
        size_t getWriteAvailable() const;
        size_t write(const float* data, size_t count);
        size_t read(float* data, size_t count);
        size_t peek(float* data, size_t count) const;
        void skip(size_t count);

    private:
        std::vector<float> buffer;
        size_t mask;
        std::atomic<size_t> writePos;
        std::atomic<size_t> readPos;
    };

class ICodec {
    public:
        virtual ~ICodec() = default;
        virtual void reset() = 0;
        virtual int getSampleRate() const = 0;
        virtual int getFrameSize() const = 0;
        virtual void processFrame(const int16_t* in, int16_t* out, bool packetLost) = 0;
        // Optional hook used by ChannelProcessor to push the current clamped
        // network parameters (packet loss rate, network degradation) into the
        // codec. Default is a no-op so existing codecs do not need to react;
        // codecs that model real packetized VoIP (e.g. OpusCodec) override it
        // to wire CTLs / internal jitter-buffer state.
        virtual void configureNetwork(float packetLossRate, float networkDegradation) {}
    };

    class WaveformConcealer {
    public:
        void reset(int frameSize);
        void storeGoodFrame(const int16_t* frame, int frameSize);
        void conceal(int16_t* out, int frameSize);

    private:
        std::vector<int16_t> history;
        float attenuation = 1.0f;
        int lastPitch = 80;

        int estimatePitch(int frameSize) const;
    };

    class G711Codec : public ICodec {
    public:
        G711Codec(int sampleRate);
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return sampleRate / 50; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
    private:
        int sampleRate;
        WaveformConcealer plc;
    };

    class GSMCodec : public ICodec {
    public:
        GSMCodec();
        ~GSMCodec();
        void reset() override;
        int getSampleRate() const override { return 8000; }
        int getFrameSize() const override { return 160; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
    private:
        void* gsmState;
        WaveformConcealer plc;
    };

    class AMRNBCodec : public ICodec {
    public:
        // dtxEnabled controls whether the bundled 3GPP AMR-NB reference
        // encoder's internal DTX/CNG path is enabled. Default is true so
        // personal / non-distribution builds use the codec's native DTX
        // (VAD + SID + comfort noise) by default. The distribution stub
        // ignores the flag entirely and keeps its pass-through behavior.
        explicit AMRNBCodec(bool dtxEnabled = true);
        ~AMRNBCodec();
        void reset() override;
        int getSampleRate() const override { return 8000; }
        int getFrameSize() const override { return 160; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
        void setDtxEnabled(bool enable);
        bool isDtxEnabled() const { return dtxEnabled; }
    private:
        void* encState;
        void* decState;
        bool dtxEnabled;
        std::vector<unsigned char> lastSerial;
        WaveformConcealer fallbackPLC;
    };

    class AMRWBCodec : public ICodec {
    public:
        // dtxEnabled controls whether the bundled 3GPP AMR-WB reference
        // encoder's internal DTX/CNG path is enabled. Default is true so
        // personal / non-distribution builds use the codec's native DTX
        // (VAD + SID + comfort noise) by default. The distribution stub
        // ignores the flag entirely and keeps its pass-through behavior.
        explicit AMRWBCodec(bool dtxEnabled = true);
        ~AMRWBCodec();
        void reset() override;
        int getSampleRate() const override { return 16000; }
        int getFrameSize() const override { return 320; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
        void setDtxEnabled(bool enable);
        bool isDtxEnabled() const { return dtxEnabled; }
    private:
        void* encState;
        void* decState;
        bool dtxEnabled;
        std::vector<unsigned char> lastSerial;
        WaveformConcealer fallbackPLC;
    };

// ---------------------------------------------------------------------------
    // EVSCodec
    //
    // Wraps the 3GPP EVS reference (TS 26.443 v12.7.0/v13.3.0) via the
    // evs_api.h C interface. Operates at 8 / 16 / 32 / 48 kHz internally with
    // 20 ms frames, so getFrameSize() returns sampleRate / 50. Configurable
    // total bitrate and bandwidth ceiling.
    // ---------------------------------------------------------------------------
    class EVSCodec : public ICodec {
    public:
        EVSCodec(int sampleRate, int bitrateBps, EVS_Bandwidth maxBw);
        ~EVSCodec() override;
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return sampleRate / 50; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;

        int getBitrate() const { return bitrateBps; }
        EVS_Bandwidth getMaxBandwidth() const { return maxBw; }

    private:
        int sampleRate;
        int bitrateBps;
        EVS_Bandwidth maxBw;

        EVS_Encoder* enc;
        EVS_Decoder* dec;

        // Reusable scratch buffers for the bitstream roundtrip.
        std::vector<unsigned char> bitstream;
        WaveformConcealer fallbackPLC;
    };

#if TELEPHONY_USE_EVS_JBM
    // ---------------------------------------------------------------------------
    // EVSCodecJbm (experimental, float EVS only, non-distribution only)
    //
    // Sibling of EVSCodec that runs the EVS encoder + Stage-1 JBM/VoIP
    // receive adapter (evs_api_rx) end-to-end inside one ICodec instance.
    //
    // Differences from EVSCodec:
    //   * No standalone EVS_Decoder. Decoding is performed inside the
    //     JBM's embedded decoder state, which also handles jitter, packet
    //     loss concealment, and time-scaling.
    //   * The encoder feeds the JBM via evs_rx_jbm_feed_frame, with the
    //     G.192 short-stream first converted to a compact MSB-first AU
    //     by evs_rx_jbm_g192_to_compact_au. The packet sequence number
    //     and the 1 ms JBM timestamp are derived from a deterministic
    //     20 ms timeline (`frameIndex * 20`).
    //   * Output is pulled per call from evs_rx_jbm_get_samples so the
    //     surrounding ChannelProcessor / ring-buffer contract (one input
    //     frame in, one output frame out) is preserved. The
    //     WaveformConcealer is the last-resort fallback for missing
    //     handles or decode errors.
    //   * DTX/CNG options match EVSCodec (dtx_enable=1, dtx_sid_interval=0,
    //     RF off, SC-VBR off). The legacy evs_enc_create entry point is
    //     the fallback if evs_enc_create_ex rejects the options.
    //
    // Only compiled when TELEPHONY_USE_EVS_JBM=ON.
    // ---------------------------------------------------------------------------
    class EVSCodecJbm : public ICodec {
    public:
        EVSCodecJbm(int sampleRate = 32000,
                    int bitrateBps = EVS_BR_13200,
                    EVS_Bandwidth maxBw = EVS_SWB);
        ~EVSCodecJbm() override;
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return sampleRate / 50; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;

        // Push the current clamped network parameters (packet loss
        // rate, network degradation) into the EVS_JBM path. Currently
        // they only drive the deterministic jitter/arrival queue
        // (cfgNetworkDegradation scales the per-packet arrival
        // offset); no encoder CTLs are issued yet because EVS_NATIVE
        // does not expose a public loss-percent / FEC control.
        void configureNetwork(float packetLossRate, float networkDegradation) override;

        int getBitrate() const { return bitrateBps; }
        EVS_Bandwidth getMaxBandwidth() const { return maxBw; }

    private:
        int sampleRate;
        int bitrateBps;
        EVS_Bandwidth maxBw;

        // EVS encoder (real 3GPP reference via evs_api.c).
        EVS_Encoder* enc;
        // Stage-1 JBM/VoIP receive adapter (evs_api_rx.c).
        EVS_RxJbm*    rx;

        // Encoder bitstream scratch. Sized to evs_max_bitstream_bytes()
        // for the configured sample rate. Holds the G.192 short-stream
        // output of evs_enc_process.
        std::vector<unsigned char> bitstream;
        // Compact MSB-first AU scratch; max 320 bytes covers the
        // 2560-bit worst case (MAX_BITS_PER_FRAME). Reused across
        // processFrame() calls.
        std::vector<unsigned char> compactAu;

        // Last-resort PLC. Stores every good decoded frame and falls
        // back to waveform repetition if the JBM or decoder is
        // unavailable / errors out.
        WaveformConcealer fallbackPLC;

        // Deterministic 20 ms timeline + RTP sequence number, both
        // advanced exactly once per input frame so the JBM sees a
        // steady, gap-free stream of packet metadata even when a
        // frame is dropped (in which case we still advance the
        // timeline but do not feed an AU).
        uint32_t frameIndex;
        uint32_t rtpSeq;

        // ----------------------------------------------------------------
        // Deterministic packet arrival / jitter queue
        //
        // Sits between the encoder and the JBM to model the per-packet
        // arrival delay that a real RTP transport would introduce. Each
        // encoded AU is wrapped in a JbmPacket whose `recvMs` is derived
        // from a deterministic LCG (jbmLcg) so the JBM never sees wall
        // clock drift and offline renders stay bit-reproducible.
        //
        // `cfgPacketLossRate` and `cfgNetworkDegradation` are the clamped
        // values last pushed in via configureNetwork(). The queue itself
        // is fed/drained purely from the 20 ms timeline, so loss / burst
        // loss come from the caller-side ChannelProcessor and are
        // observed here as `packetLost` frames (which skip enqueue).
        // ----------------------------------------------------------------

        // Maximum number of buffered packets. Larger values waste
        // memory; smaller values can starve the JBM when degradation is
        // high. 16 covers the worst case (a few frames of jitter at 50
        // fps) plus headroom for a small burst.
        static constexpr int kMaxQueueSize = 16;

        // One simulated RTP packet waiting in the arrival queue.
        struct JbmPacket {
            std::vector<unsigned char> au;   // compact MSB-first AU bytes
            int      auBits;                 // AU length in bits (1..2560)
            uint16_t seq;                    // 16-bit RTP sequence number
            uint32_t rtpTsMs;                // RTP timestamp on the JBM's 1 ms scale
            uint32_t recvMs;                 // wall-clock arrival time in ms
        };

        // FIFO of pending packets, oldest first. Drained by jbmDrain()
        // before each encode/enqueue step.
        std::vector<JbmPacket> jbmQueue;

        // Deterministic linear congruential generator state used by
        // jbmArrivalOffsetMs() so jitter is reproducible across runs.
        uint32_t jbmLcg;

        // Clamped network parameters pushed in by configureNetwork().
        // Preserved across reset() so a "new session" doesn't reset the
        // user's network dial.
        float cfgPacketLossRate;
        float cfgNetworkDegradation;

        // Derive a non-negative per-packet arrival offset (ms) from
        // jbmLcg, scaled by cfgNetworkDegradation. Degradation 0
        // returns 0 (or minimal jitter) so a clean path behaves
        // exactly as the previous direct-feed code did.
        int jbmArrivalOffsetMs();

        // Copy `auBits` / ceil(auBits/8) bytes into a JbmPacket, stamp
        // `recvMs = rtpTsMs + offset`, push into jbmQueue, then trim.
        void jbmEnqueue(const unsigned char* au,
                        int auBits,
                        uint16_t seq,
                        uint32_t rtpTsMs);

        // Feed every queued packet whose recvMs <= sysMs into the JBM,
        // oldest first. Errors from individual feeds are ignored; the
        // JBM will conceal any slots that didn't make it in.
        void jbmDrain(uint32_t sysMs);

        // Cap jbmQueue to at most kMaxQueueSize by dropping the oldest
        // packets. Insertion order is ascending by rtpTsMs because we
        // only append and `recvMs >= rtpTsMs`, so the head of the
        // vector is always the oldest.
        void jbmTrim();
    };
#endif // TELEPHONY_USE_EVS_JBM

    // ---------------------------------------------------------------------------
    // OpusCodec (experimental, non-distribution)
    //
    // Wraps the BSD-licensed Opus encoder/decoder from xiph/opus via its C
    // API. Defaults are VOIP-friendly: 48 kHz, 20 ms frames (960 samples),
    // application VOIP, 24 kbps target bitrate, moderate complexity.
    //
    // On top of the bare encoder/decoder round-trip this class simulates a
    // small packetized VoIP transport so the path is closer to what an RTP
    // pipeline would see:
    //   - encoder CTLs are configured via configureNetwork(): FEC on, packet
    //     loss percent driven by the configured loss rate / degradation
    //   - each input frame becomes an Opus packet with an increasing sequence
    //     number; packets have a deterministic arrival frame derived from a
    //     small LCG seeded by networkDegradation
    //   - the playback target lags by a small base delay plus a degradation-
    //     dependent extra; if the target packet is available we decode it,
    //     otherwise we fall back to in-band FEC (decode_fec=1) on the next
    //     available packet, then Opus's built-in PLC, then the
    //     WaveformConcealer
    //   - the queue is capped and stale packets are dropped on each step so
    //     the transport stays deterministic and bounded
    //
    // One output frame is still produced per input frame, so the surrounding
    // ChannelProcessor / ring-buffer contract is unchanged.
    //
    // Only compiled into TelephonyDSP when TELEPHONY_EXPERIMENTAL_NETWORK=1.
    // ---------------------------------------------------------------------------
    class OpusCodec : public ICodec {
    public:
        OpusCodec(int sampleRate, int bitrateBps, int complexity);
        ~OpusCodec() override;
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return sampleRate / 50; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
        void configureNetwork(float packetLossRate, float networkDegradation) override;

        int getBitrate() const { return bitrateBps; }

    private:
        int sampleRate;
        int bitrateBps;
        int frameSize;     // samples per 20 ms frame at sampleRate
        int complexity;
        void* encoder;     // OpusEncoder* (kept void* to avoid pulling opus.h into the header)
        void* decoder;     // OpusDecoder*
        std::vector<unsigned char> bitstream;
        WaveformConcealer fallbackPLC;

        // --- Packetized VoIP simulation state ---
        //
        // We never have more than this many buffered packets. Larger values
        // waste memory; smaller values can starve Opus's PLC / FEC path.
        static constexpr int kMaxQueueSize = 16;

        // One transport packet: bitstream bytes plus sequence number and
        // arrival frame (the playback-frame index at which it should become
        // available to the decoder).
        struct VoipPacket {
            uint32_t seq;            // monotonically increasing per encoded frame
            int      arrivalFrame;   // playback frame index at which it arrives
            std::vector<unsigned char> data;
        };

        // Per-packet entry in the simulated network queue.
        std::vector<VoipPacket> queue;

        // Sequence number assigned to the next encoded packet.
        uint32_t nextSeq;
        // Playback frame counter; the codec emits one decoded frame per
        // call, so this advances by exactly 1 each processFrame().
        int      playbackFrame;

        // Current clamped network parameters (mirrors what
        // ChannelProcessor::configure pushed in).
        float cfgPacketLossRate;
        float cfgNetworkDegradation;

        // Deterministic LCG state used to derive per-packet arrival jitter.
        uint32_t jitterLcg;

        // Initial Playback-target lag: this many decoded frames must be
        // queued before we start playing back. Includes a degradation-
        // dependent extra so higher degradation => more buffering.
        int basePlaybackDelay() const;
        // Map the network parameters to Opus's PACKET_LOSS_PERC value.
        int derivedPacketLossPercent() const;
        // Apply encoder CTLs based on the current cfgPacketLossRate /
        // cfgNetworkDegradation. Safe to call multiple times.
        void applyNetworkCtls();
        // Derive the arrival frame for a packet with `seq` using the LCG
        // state. The state is advanced as a side effect so each call is
        // deterministic given the seed.
        int arrivalFrameFor(uint32_t seq);
        // Drop any packet whose sequence is older than
        // (nextSeq - kMaxQueueSize); used to keep the queue bounded.
        void trimQueue();
    };

    // ---------------------------------------------------------------------------
    // SpeexDSPAux (experimental, non-distribution)
    //
    // Lightweight helper around the BSD-licensed speex_preprocess / jitter
    // buffer APIs. The point of this first step is to compile SpeexDSP into
    // TelephonyDSP and prove the headers / link line; it does not change
    // audio output yet. A follow-up step can:
    //   - use speex_preprocess_run() to compute VAD probability and decide
    //     when to emit DTX/SID frames for OPUS_VOIP / EVS_LIKE
    //   - drive a SpeexJitter wrapper around OPUS_VOIP packets
    //   - generate low-level comfort noise for SILENCE frames
    //
    // Exposed here so future iterations can plumb it into ChannelProcessor
    // without re-dealing with CMake / include paths.
    // ---------------------------------------------------------------------------
    class SpeexDSPAux {
    public:
        SpeexDSPAux();
        ~SpeexDSPAux();
        void configure(int sampleRate, int frameSize);
        void reset();
        // Returns speex_preprocess_ctl(SPEEX_PREPROCESS_GET_PROB) - speech
        // probability in [0,1] (SpeexDSP reports it as a percent in [0,100]
        // and we rescale). Returns -1.0 when SpeexDSP is not compiled in or
        // when no frame has been processed yet. Currently only meaningful
        // if SPEEX_PREPROCESS_SET_VAD has been enabled in configure(); with
        // VAD disabled, the underlying value is not populated and callers
        // should treat the result as "unknown".
        float getSpeechProbability() const;
        // Returns true if the last run() call detected voice activity.
        // Always returns false when SpeexDSP is not compiled in, when the
        // helper is not configured, or while SPEEX_PREPROCESS_SET_VAD is
        // disabled in configure().
        bool lastFrameIsSpeech() const;
        void runPreprocess(int16_t* frame); // in-place, no-op when disabled
    private:
 void* state; // SpeexPreprocessState* kept void* to avoid speex headers here
 int sampleRate;
 int frameSize;
 bool configured;
// Tracks whether SPEEX_PREPROCESS_SET_VAD is enabled on `state`.
  // SpeexDSP's VAD is currently a placeholder that logs
  // "The VAD has been replaced by a hack pending a complete rewrite"
  // every time it is enabled, so we leave it off and expose it as a
  // separate flag rather than a derived state. While this is false,
  // getSpeechProbability() returns -1.0f ("unknown") and
  // lastFrameIsSpeech() returns false, instead of a `0.0f / false`
  // pair that would falsely imply "definitely not speech".
 bool vadEnabled;
 };

    class ChannelProcessor {
    public:
        ChannelProcessor(double hostSR);
        ~ChannelProcessor();

        void setSampleRate(double sr);
        void setMode(EraMode mode);
        void setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw);
        void reset();
        void pushInput(const float* in, int numSamples);
        size_t pullOutput(float* out, int numSamples);
        size_t getAvailableOutput() const;
        void configure(bool artifacts, float amount, float packetLossRate, float networkDegradation);

    private:
        double hostSampleRate;
        EraMode currentMode;
        bool paramArtifactsEnabled;
        float paramArtifactAmount;
        float paramPacketLossRate;
        float paramNetworkDegradation;
        uint32_t packetLossSeed;
        int packetLossBurstFrames;

        // EVS configuration (only used when currentMode == EVS_NATIVE)
        int evsSampleRate;
        int evsBitrateBps;
        EVS_Bandwidth evsMaxBandwidth;

        RingBuffer ringCodecIn;
        RingBuffer ringCodecOut;
        RingBuffer outputBuffer;

        std::unique_ptr<r8b::CDSPResampler24> resamplerDown;
        std::unique_ptr<r8b::CDSPResampler24> resamplerUp;
        std::unique_ptr<ICodec> codec;

        // Maximum input length that was used to construct the resamplers.
        // Callers of resamplerDown/resamplerUp must never pass more than this
        // many input samples in a single r8brain::CDSPResampler24::process()
        // call, otherwise r8brain's pre-allocated internal buffers overflow.
        // We track both values so we can chunk process() calls safely.
        int downMaxInLen;
        int upMaxInLen;

        // Experimental SpeexDSP-backed VAD/DTX helper. Configured lazily when
        // a codec rate becomes available; reset() touches it but it does not
        // influence the audio path yet. Only present when
        // TELEPHONY_EXPERIMENTAL_NETWORK is enabled.
        std::unique_ptr<SpeexDSPAux> speexAux;

        // Buffers
        std::vector<double> resampInBuf;
        std::vector<float> codecFrameF;
        std::vector<int16_t> codecFrameSIn;
        std::vector<int16_t> codecFrameSOut;
        std::vector<float> tempProcessBuf;
        // Per-chunk staging for the upsampler output. r8brain's process()
        // returns a pointer to an internal buffer that is invalidated by the
        // next process() call, so we must copy each chunk out before the
        // next iteration of the chunking loop.
        std::vector<float> resampUpOutBuf;
        WaveformConcealer simulatedPathPLC;

        // Filters for G.711 (Cascaded for 24dB/oct)
        Biquad hpFilter1, hpFilter2;
        Biquad lpFilter1, lpFilter2;

        void recreateResamplers();
        void recreateCodec();
        void prepareInternalBuffers(int maxBlockSize);
        void updateFilters();
        float nextPacketRandom();
        bool shouldDropPacket();

        void processG711(int numSamples, const float* in, float* out);
        void processEVSLike(int numSamples, const float* in, float* out);
        void processCodec(int numSamples, const float* in);
        void applyArtifacts(float* buffer, int numSamples);
    };

    class SignalProcessor {
    public:
        SignalProcessor();
        ~SignalProcessor();

        void setSampleRate(double sampleRate);
        void setMode(EraMode mode);
        void setRoute(RouteEndpoint input, RouteEndpoint output, DegradationSegment degradationSegment);
        void setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw);
        void setParameters(float dryWet, float outGaindB, bool artifacts, float artifactAmount,
                           float packetLossRate = 0.0f, float networkDegradation = 0.0f);
        void setSimulateLatency(bool enable);
        void reset();
        void process(float** inputs, int numIns, float** outputs, int numOuts, int numSamples);
        int getLatencySamples() const;

    private:
        double hostSampleRate;
        EraMode currentMode;
        bool routeModelEnabled;
        RouteEndpoint inputEndpoint;
        RouteEndpoint outputEndpoint;
        DegradationSegment degradationSegment;
        float paramDryWet;
        float paramOutGain;
        bool paramArtifactsEnabled;
        float paramArtifactAmount;
        float paramPacketLossRate;
        float paramNetworkDegradation;
        bool simulateLatency;

        // EVS configuration (only used when currentMode == EVS_NATIVE)
        int evsSampleRate;
        int evsBitrateBps;
        EVS_Bandwidth evsMaxBandwidth;

        int targetLatencySamples;
        int64_t inTotalSamples;
        int64_t outTotalSamples;

        std::vector<std::unique_ptr<ChannelProcessor>> inputLegs;
        std::vector<std::unique_ptr<ChannelProcessor>> outputLegs;
        std::vector<std::unique_ptr<RingBuffer>> dryBuffers;

        void updateLatency();
        void ensureChannels(int count);
        EraMode endpointToMode(RouteEndpoint endpoint) const;
        bool degradesInputLeg() const;
        bool degradesOutputLeg() const;
        void applyRouteToChannels();
    };

}
