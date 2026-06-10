#pragma once

#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"
#include <vector>

namespace TelephonyDSP {

    // Opus bandwidth constants (mirrors opus_defines.h OPUS_BANDWIDTH_*).
    // Duplicated locally so the header doesn't have to pull in <opus.h>;
    // the values are part of the stable Opus public ABI.
    constexpr int kOpusBandwidthNarrowband   = 1101; // 4 kHz
    constexpr int kOpusBandwidthMediumband   = 1102; // 6 kHz
    constexpr int kOpusBandwidthWideband     = 1103; // 8 kHz
    constexpr int kOpusBandwidthSuperwideband= 1104; // 12 kHz
    constexpr int kOpusBandwidthFullband     = 1105; // 20 kHz (default)

    class OpusCodec : public ICodec {
    public:
        OpusCodec(int sampleRate, int bitrateBps, int complexity,
                  int maxBw = kOpusBandwidthFullband);
        ~OpusCodec() override;
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return sampleRate / 50; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
        void configureNetwork(float packetLossRate, float networkDegradation) override;

        // Update the target encoder bitrate at runtime. `bps` is clamped to
        // Opus's legal range (6..510000) before being stored and pushed into
        // the live encoder. Safe to call before the encoder exists; the
        // value is then applied the next time the encoder is (re)created
        // (see ctor and applyNetworkCtls()).
        void setBitrate(int bps);

        int getBitrate() const { return bitrateBps; }

        // Set the maximum audio bandwidth the encoder is allowed to use.
        // Mirrors OPUS_SET_MAX_BANDWIDTH. Defaults to FB (1105) which
        // preserves the previous implicit behavior. Safe to call before
        // the encoder exists (the value is cached and applied once
        // recreateCodec-style creation completes).
        void setMaxBandwidth(int bw);
        int  getMaxBandwidth() const { return maxBandwidth; }

    private:
        int sampleRate;
        int bitrateBps;
        int frameSize;     // samples per 20 ms frame at sampleRate
        int complexity;
        int maxBandwidth;  // current OPUS_BANDWIDTH_* value (e.g. 1105 = FB)
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

        // RF channel-aware feedback state (13.2 kbps / >= 16 kHz only).
        bool rfActive;                     // true if RF is viable at ctor time
        int  lastAppliedFecOffset;          // throttling: last value pushed to encoder
        int  lastAppliedFecHi;             // throttling: last HI/LO pushed

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

} // namespace TelephonyDSP
