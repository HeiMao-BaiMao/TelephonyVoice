#pragma once

#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"
#include "evs_api.h"
#include "evs_api_rx.h"

namespace TelephonyDSP {

    class EVSCodecJbm : public ICodec {
    public:
        EVSCodecJbm(int sampleRate = 32000,
                    int bitrateBps = EVS_BR_13200,
                    EVS_Bandwidth maxBw = EVS_SWB,
                    int dtxSidInterval = 0,
                    bool scVbrEnabled = false);
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

        // Update the DTX SID update interval. 0 = variable (codec default,
        // promoted internally to ~12 frames by init_encoder). 3..100 = fixed
        // SID update interval in 20 ms frames. Values outside the supported
        // set are clamped to 0 to avoid evs_enc_create_ex rejecting the
        // configuration. The new value is applied immediately by tearing
        // down and re-creating the encoder through reset().
        void setDtxSidInterval(int interval);

        // Toggle EVS Source-Controlled VBR (sc_vbr_enable). The value is
        // persisted on the instance and applied the next time reset() (or
        // the ctor) builds the encoder, so callers do not need to time
        // the call against the audio thread.
        void setScVbrEnabled(bool enable) { if (scVbrEnabled != enable) { scVbrEnabled = enable; reset(); } }
        bool getScVbrEnabled() const { return scVbrEnabled; }

    private:
        int sampleRate;
        int bitrateBps;
        EVS_Bandwidth maxBw;
        // 0 = variable SID interval (default), 3..100 = fixed frames.
        int dtxSidInterval;

        // EVS Source-Controlled VBR toggle. Mirrored into the encoder's
        // EVS_EncOptions every time the encoder is (re)created. Persisted
        // across reset() so a later setScVbrEnabled() survives a session
        // restart.
        bool scVbrEnabled;

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

        // RF channel-aware feedback state (13.2 kbps / >= 16 kHz only).
        bool rfActive;                     // true if RF is viable at ctor time
        int  lastAppliedFecOffset;          // throttling: last value pushed to encoder
        int  lastAppliedFecHi;             // throttling: last HI/LO pushed

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

} // namespace TelephonyDSP
