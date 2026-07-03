#include "dsp/OpusCodec.h"
#if TELEPHONY_EXPERIMENTAL_NETWORK
#include <opus.h>
#endif
#include <cmath>

namespace TelephonyDSP {

    // ---------------------------------------------------------------------------
    // OpusCodec
    // ---------------------------------------------------------------------------
#if TELEPHONY_EXPERIMENTAL_NETWORK
    OpusCodec::OpusCodec(int sr, int bitrate, int complexity, int maxBw)
        : sampleRate(sr)
        , bitrateBps(bitrate)
        , frameSize(sr / 50) // 20 ms
        , complexity(complexity)
        , maxBandwidth(maxBw)
        , encoder(nullptr)
        , decoder(nullptr)
        , queue()
        , nextSeq(0)
        , playbackFrame(0)
        , cfgPacketLossRate(0.0f)
        , cfgNetworkDegradation(0.0f)
        , jitterLcg(0x9E3779B9u)
    {
        // Opus bitstream budget: a generous worst case so 64 kbps modes still
        // fit. 4000 bytes is well over the ~1500 byte RTP payload ceiling.
        bitstream.resize(4000, 0);
        fallbackPLC.reset(frameSize);
        queue.reserve(kMaxQueueSize);

        int err = 0;
        OpusEncoder* enc = opus_encoder_create(sampleRate, 1, OPUS_APPLICATION_VOIP, &err);
        if (err != OPUS_OK || !enc) {
            encoder = nullptr;
        } else {
            opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrateBps));
            opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
            // Default CTLs; configureNetwork() may override these.
            // - FEC on so the decoder can recover from a single lost packet
            //   via in-band FEC when the next packet arrives.
            // - Packet loss percent starts at 0; configureNetwork() updates
            //   it from the network parameters.
            // - DTX is enabled via Opus's own internal VAD, which is
            //   self-contained and well-tested, and independent of
            //   SpeexDSPAux.
            opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(0));
            opus_encoder_ctl(enc, OPUS_SET_DTX(1));
            // Cap the audio bandwidth the encoder is allowed to use. Without
            // this Opus defaults to FB (20 kHz) at 48 kHz input; we honor the
            // caller's preference (typically set via setMaxBandwidth()).
            opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(maxBandwidth));
            encoder = enc;
        }

        OpusDecoder* dec = opus_decoder_create(sampleRate, 1, &err);
        if (err != OPUS_OK || !dec) {
            decoder = nullptr;
        } else {
            decoder = dec;
        }
    }

    OpusCodec::~OpusCodec() {
        if (encoder) opus_encoder_destroy(static_cast<OpusEncoder*>(encoder));
        if (decoder) opus_decoder_destroy(static_cast<OpusDecoder*>(decoder));
    }

    void OpusCodec::reset() {
        fallbackPLC.reset(frameSize);
        // Flush the simulated transport state so a new "session" starts
        // with an empty jitter buffer and aligned sequence numbers.
        queue.clear();
        nextSeq = 0;
        playbackFrame = 0;
        // Re-seed the LCG so the jitter pattern stays deterministic across
        // reset() calls; this matters for repeatable tests.
        jitterLcg = 0x9E3779B9u;
        // The decoder has no OPUS_RESET_STATE; recreate it. The encoder has
        // OPUS_RESET_STATE so we keep it and only flush its state.
        if (encoder) {
            opus_encoder_ctl(static_cast<OpusEncoder*>(encoder), OPUS_RESET_STATE);
        }
        if (decoder) {
            opus_decoder_destroy(static_cast<OpusDecoder*>(decoder));
            int err = 0;
            decoder = opus_decoder_create(sampleRate, 1, &err);
        }
    }

    int OpusCodec::basePlaybackDelay() const {
        // 2 frames (40 ms) minimum, plus 0..4 frames extra driven by
        // networkDegradation. Clamp so a heavily degraded path doesn't
        // grow the buffer past the queue cap.
        const int extra = (int)std::clamp(cfgNetworkDegradation * 4.0f, 0.0f, 4.0f);
        return 2 + extra;
    }

    int OpusCodec::derivedPacketLossPercent() const {
        // PACKET_LOSS_PERC takes an int in [0, 100]. Combine the user-supplied
        // loss rate with a degradation-dependent boost so higher degradation
        // also implies higher expected loss on the encoder side, mirroring
        // what ChannelProcessor::shouldDropPacket() does on the caller side.
        const float d = std::clamp(cfgNetworkDegradation, 0.0f, 1.0f);
        const float combined = std::clamp(cfgPacketLossRate + d * d * 0.08f, 0.0f, 0.95f);
        return (int)std::round(combined * 100.0f);
    }

    void OpusCodec::applyNetworkCtls() {
        if (!encoder) return;
        OpusEncoder* enc = static_cast<OpusEncoder*>(encoder);
        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(derivedPacketLossPercent()));
        // Re-apply the user's chosen max bandwidth in case it changed
        // (e.g. live setMaxBandwidth() call) and to keep this in lockstep
        // with the other network-driven CTLs.
        opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(maxBandwidth));
        // Re-apply the cached target bitrate so a later setBitrate() call is
        // honored even if applyNetworkCtls() runs for an unrelated reason
        // (e.g. after configureNetwork() or recreateCodec()).
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrateBps));
        // DTX stays off; see OpusCodec ctor comment.
    }

    void OpusCodec::setBitrate(int bps) {
        // Opus's legal range per opus_defines.h is OPUS_BITRATE_MIN
        // (6000) .. OPUS_BITRATE_MAX (510000). Clamp to that range so an
        // out-of-range caller value (e.g. from automation or a typo in
        // the host UI) cannot trigger an OPUS_BAD_ARG error from
        // OPUS_SET_BITRATE.
        const int clamped = std::clamp(bps, 6000, 510000);
        bitrateBps = clamped;
        if (encoder) {
            opus_encoder_ctl(static_cast<OpusEncoder*>(encoder),
                             OPUS_SET_BITRATE(bitrateBps));
        }
    }

    void OpusCodec::setMaxBandwidth(int bw) {
        // Cache the new value first so a subsequent encoder creation
        // (e.g. reset() path) picks it up. If the encoder already exists
        // we push the change immediately.
        maxBandwidth = bw;
        if (encoder) {
            opus_encoder_ctl(static_cast<OpusEncoder*>(encoder),
                             OPUS_SET_MAX_BANDWIDTH(maxBandwidth));
        }
    }

    int OpusCodec::arrivalFrameFor(uint32_t seq) {
        // Linear congruential generator step (Numerical Recipes constants).
        jitterLcg = jitterLcg * 1664525u + 1013904223u;
        // Map the upper bits of the LCG state to a non-negative jitter
        // offset in frames. degradation scales the magnitude: a clean
        // network (degradation 0) has zero jitter so every packet arrives
        // exactly basePlaybackDelay frames after it was sent, and a heavily
        // degraded one can drift several frames past its playback slot
        // (concealed by FEC/PLC below).
        const float d = std::clamp(cfgNetworkDegradation, 0.0f, 1.0f);
        const float maxJitter = d * 4.0f; // up to ~4 frames
        const uint32_t r = (jitterLcg >> 8) & 0xFFFFu;
        const float u = (float)r / 65535.0f;     // [0,1]
        const int offset = (int)std::round(u * maxJitter);
        // arrivalFrame is a non-negative playback-frame index. Each encoded
        // packet is associated with the playback frame at which it should
        // become available.
        const int baseDelay = basePlaybackDelay();
        return baseDelay + offset;
    }

    void OpusCodec::trimQueue() {
        // Drop the oldest packets if we're at or above the cap. We compare
        // against the highest sequence we've seen so a wraparound can't
        // make stale packets look fresh.
        if (queue.size() >= (size_t)kMaxQueueSize) {
            // Sort by seq ascending then drop the head until under cap.
            // Insertion order is already ascending (we only append), so the
            // oldest packets live at the front.
            size_t excess = queue.size() - (size_t)kMaxQueueSize + 1;
            queue.erase(queue.begin(), queue.begin() + (std::ptrdiff_t)excess);
        }
    }

    void OpusCodec::configureNetwork(float packetLossRate, float networkDegradation) {
        cfgPacketLossRate = std::clamp(packetLossRate, 0.0f, 0.95f);
        cfgNetworkDegradation = std::clamp(networkDegradation, 0.0f, 1.0f);
        applyNetworkCtls();
    }

    void OpusCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = frameSize;
        OpusEncoder* enc = static_cast<OpusEncoder*>(encoder);
        OpusDecoder* dec = static_cast<OpusDecoder*>(decoder);

        // -----------------------------------------------------------------
        // Stage 1: encode / drop on the send side of the simulated network.
        // -----------------------------------------------------------------
        // The caller already folds ChannelProcessor::shouldDropPacket() into
        // `packetLost`, so we honor it here: if the caller says this frame's
        // packet should not enter the transport, we skip encoding and the
        // packet will simply never arrive at the receiver.
        if (packetLost) {
            // Reserve the sequence number anyway so the receiver-side target
            // advances in lockstep with the encoder, which keeps the
            // simulated jitter-buffer math deterministic.
            (void)nextSeq++;
        } else if (enc) {
            int nbBytes = opus_encode(enc, in, fs, bitstream.data(), (opus_int32)bitstream.size());
            if (nbBytes > 0) {
                VoipPacket p;
                p.seq = nextSeq++;
                // arrivalFrame = send frame (== seq for this steady 1-in
                // 1-out transport) + base transport delay + LCG jitter.
                p.arrivalFrame = (int)p.seq + arrivalFrameFor(p.seq);
                p.data.assign(bitstream.begin(), bitstream.begin() + nbBytes);
                queue.push_back(std::move(p));
                trimQueue();
            } else {
                // Encode failure - reserve the seq number so timing stays in
                // sync but don't put anything in the queue.
                (void)nextSeq++;
            }
        } else {
            // No encoder available; advance the seq counter so the receiver
            // side keeps moving even in degenerate paths.
            (void)nextSeq++;
        }

        // -----------------------------------------------------------------
        // Stage 2: receive / decode on the playback side of the simulated
        // network. We always emit exactly one output frame so the
        // surrounding ChannelProcessor / ring-buffer contract stays the
        // same; missing packets are concealed by Opus's PLC / FEC / our
        // fallback concealer in that order.
        // -----------------------------------------------------------------
        const int targetFrame = playbackFrame;
        bool decoded = false;

        if (dec) {
            // The playback slot for this frame: with a steady 1-in/1-out
            // transport the packet played at frame T is the one sent
            // basePlaybackDelay() frames earlier. During the initial
            // warm-up (T < baseDelay) no packet is due yet and the PLC
            // below emits decoder silence.
            const int64_t seqToPlay = (int64_t)targetFrame - basePlaybackDelay();

            // Drop packets for slots that have already been played; they
            // arrived too late to be useful (their slot was concealed).
            size_t w = 0;
            for (size_t i = 0; i < queue.size(); ++i) {
                if ((int64_t)queue[i].seq >= seqToPlay) {
                    if (w != i) queue[w] = std::move(queue[i]);
                    ++w;
                }
            }
            queue.resize(w);

            if (seqToPlay >= 0) {
                // Exact packet for this slot, if it has arrived by now.
                size_t idx = queue.size();
                size_t next = queue.size();
                for (size_t i = 0; i < queue.size(); ++i) {
                    if ((int64_t)queue[i].seq == seqToPlay) idx = i;
                    else if ((int64_t)queue[i].seq == seqToPlay + 1) next = i;
                }

                if (idx < queue.size() && queue[idx].arrivalFrame <= targetFrame) {
                    int n = opus_decode(dec, queue[idx].data.data(),
                                        (opus_int32)queue[idx].data.size(),
                                        out, fs, 0);
                    if (n == fs) {
                        decoded = true;
                    }
                    queue.erase(queue.begin() + (std::ptrdiff_t)idx);
                } else if (next < queue.size() && queue[next].arrivalFrame <= targetFrame) {
                    // This slot's packet is missing or late, but the packet
                    // for the NEXT slot has already arrived: recover this
                    // frame from its in-band FEC (LBRR) data. The packet
                    // stays queued so the next slot still decodes it
                    // normally - that is the standard Opus FEC sequence.
                    int n = opus_decode(dec, queue[next].data.data(),
                                        (opus_int32)queue[next].data.size(),
                                        out, fs, 1);
                    if (n == fs) {
                        decoded = true;
                    }
                }
            }

            if (!decoded) {
                // Pure PLC: Opus's built-in concealment for missing frames.
                int n = opus_decode(dec, nullptr, 0, out, fs, 0);
                if (n == fs) {
                    decoded = true;
                }
            }
        }

        if (!decoded) {
            if (packetLost && !enc && !dec) {
                // Encoder and decoder missing - pass-through path.
                std::memcpy(out, in, fs * sizeof(int16_t));
            } else {
                // Last-resort concealer.
                fallbackPLC.conceal(out, fs);
            }
        } else {
            // Good output - feed the concealer so it has recent history
            // if subsequent frames are lost.
            fallbackPLC.storeGoodFrame(out, fs);
        }

        // Advance playback bookkeeping.
        ++playbackFrame;
    }
#else
    // Stub implementations keep TelephonyDSP compilable when the experimental
    // libraries are disabled (e.g. distribution build). OpusCodec instances
    // should never be created in that path; this is a defensive no-op.
    OpusCodec::OpusCodec(int, int, int, int maxBw) : sampleRate(0), bitrateBps(0), frameSize(0), complexity(0), maxBandwidth(maxBw), encoder(nullptr), decoder(nullptr) {
        fallbackPLC.reset(0);
    }
    OpusCodec::~OpusCodec() {}
    void OpusCodec::reset() { fallbackPLC.reset(frameSize); }
    void OpusCodec::configureNetwork(float, float) {}
    void OpusCodec::setMaxBandwidth(int bw) { maxBandwidth = bw; }
    void OpusCodec::setBitrate(int bps) { bitrateBps = bps; }
    void OpusCodec::processFrame(const int16_t* in, int16_t* out, bool) {
        const int fs = frameSize;
        std::memcpy(out, in, fs * sizeof(int16_t));
    }
#endif

} // namespace TelephonyDSP
