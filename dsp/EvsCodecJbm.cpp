#include "dsp/EvsCodecJbm.h"
#include <cmath>

namespace TelephonyDSP {

    // ---------------------------------------------------------------------------
    // EVSCodecJbm (experimental, float EVS only, non-distribution only)
    //
    // Encoder + Stage-1 JBM/VoIP adapter in one ICodec. See the header
    // comment for the full design. The implementation follows
    // EVSCodec's DTX/RF/SC-VBR options and uses the public
    // evs_api_rx / evs_api_rx_smoke-compatible helpers; no submodule
    // edits are required.
    // ---------------------------------------------------------------------------
#if TELEPHONY_USE_EVS_JBM
#if !defined(TELEPHONY_DISTRIBUTION_BUILD)
    EVSCodecJbm::EVSCodecJbm(int sampleRate, int bitrateBps, EVS_Bandwidth maxBw)
        : sampleRate(sampleRate)
        , bitrateBps(bitrateBps)
        , maxBw(maxBw)
        , enc(nullptr)
        , rx(nullptr)
        , frameIndex(0)
        , rtpSeq(0)
        , jbmQueue()
        , jbmLcg(0x9E3779B9u)            // golden-ratio LCG seed; same as OpusCodec
        , cfgPacketLossRate(0.0f)
        , cfgNetworkDegradation(0.0f)
    {
        fallbackPLC.reset(getFrameSize());

        // Reserve queue capacity so the FIFO never reallocates during
        // steady-state operation. Bounded by kMaxQueueSize.
        jbmQueue.reserve(kMaxQueueSize);

        // Match EVSCodec's DTX options. The JBM does not see DTX/SID
        // frames specially (they are still valid AUs in the JBM's MSB-
        // first compact format), so the encoder's DTX/CNG output is
        // pushed through unchanged.
        EVS_EncOptions opts;
        evs_enc_options_init(&opts);
        opts.dtx_enable       = 1;
        opts.dtx_sid_interval = 0;     // variable SID (see evs_api.h)
        opts.rf_enable        = 0;  // RF enabled later via evs_enc_set_rf
        opts.sc_vbr_enable    = 0;
        rfActive = (bitrateBps == EVS_BR_13200 && sampleRate >= 16000);
        lastAppliedFecOffset = -1;
        lastAppliedFecHi     = -1;
        enc = evs_enc_create_ex(sampleRate, bitrateBps, maxBw, &opts);
        if (!enc) {
            // Fall back to the legacy entry point on configuration
            // rejection so the codec still encodes.
            enc = evs_enc_create(sampleRate, bitrateBps, maxBw);
        }

        // Stage-1 JBM/VoIP receive adapter. jbm_safety_margin_ms=0 is
        // clamped to the reference default (60 ms) inside the adapter.
        rx = evs_rx_jbm_create(sampleRate, bitrateBps, 0);

        // Encoder bitstream scratch (G.192 short-stream), sized to the
        // worst-case AU at the configured sample rate.
        bitstream.resize(evs_max_bitstream_bytes(sampleRate));
        // Compact MSB-first AU scratch: 320 bytes covers the 2560-bit
        // worst case (MAX_BITS_PER_FRAME). The helper's own
        // EVS_RX_G192_MAX_AU_BYTES is 320, so we match that exactly.
        compactAu.assign(320, 0);
    }

    EVSCodecJbm::~EVSCodecJbm() {
        if (enc) { evs_enc_destroy(enc); enc = nullptr; }
        if (rx)  { evs_rx_jbm_destroy(rx); rx = nullptr; }
    }

    void EVSCodecJbm::reset() {
        fallbackPLC.reset(getFrameSize());
        frameIndex = 0;
        rtpSeq     = 0;

        // Flush the deterministic arrival queue and re-seed the LCG so
        // a new session starts with an empty jitter buffer and the
        // same reproducible jitter pattern as the constructor.
        // cfgPacketLossRate / cfgNetworkDegradation are preserved:
        // they are the user's network dial, not transport state.
        jbmQueue.clear();
        jbmLcg = 0x9E3779B9u;

        // Tear down + recreate the encoder (with the same DTX options
        // used in the ctor).
        if (enc) { evs_enc_destroy(enc); enc = nullptr; }
        EVS_EncOptions opts;
        evs_enc_options_init(&opts);
        opts.dtx_enable       = 1;
        opts.dtx_sid_interval = 0;
        opts.rf_enable        = (bitrateBps == EVS_BR_13200 && sampleRate >= 16000) ? 1 : 0;
        opts.rf_fec_offset   = 0;     // use FEC_OFFSET default (3)
        opts.rf_fec_hi       = 1;
        opts.sc_vbr_enable    = 0;
        const bool rfOk = (opts.rf_enable == 1);
        rfActive             = rfOk;
        lastAppliedFecOffset = -1;    // force first apply
        lastAppliedFecHi     = -1;
        enc = evs_enc_create_ex(sampleRate, bitrateBps, maxBw, &opts);
        if (!enc) {
            enc = evs_enc_create(sampleRate, bitrateBps, maxBw);
        }

        // Tear down + recreate the JBM. EVS_RX_Close destroys the
        // embedded Decoder_State contents; the adapter frees its
        // own Decoder_State allocation, so this is a full reset.
        if (rx) { evs_rx_jbm_destroy(rx); rx = nullptr; }
        rx = evs_rx_jbm_create(sampleRate, bitrateBps, 0);
    }

    void EVSCodecJbm::configureNetwork(float packetLossRate, float networkDegradation) {
        // Match ChannelProcessor's clamping so the configured values
        // here cannot disagree with the ones the caller pushed in.
        cfgPacketLossRate      = std::clamp(packetLossRate, 0.0f, 0.95f);
        cfgNetworkDegradation  = std::clamp(networkDegradation, 0.0f, 1.0f);
        // No encoder CTLs are issued here yet: EVS_NATIVE's public C
        // API does not expose a packet-loss-percent or FEC-offset
        // control, and the queue already observes loss via the
        // caller's `packetLost` flag.
    }

    int EVSCodecJbm::jbmArrivalOffsetMs() {
        // Numerical-Recipes LCG step. jbmLcg is mutated as a side
        // effect so the sequence is reproducible across runs given the
        // same seed.
        jbmLcg = jbmLcg * 1664525u + 1013904223u;

        // Scale the per-packet arrival jitter window by the user-set
        // network degradation. degradation 0 -> 0 ms jitter so the
        // transport is effectively zero-delay and the previous
        // direct-feed behavior is preserved exactly.
        const float d = std::clamp(cfgNetworkDegradation, 0.0f, 1.0f);
        const int maxJitterMs = (int)std::round(d * 100.0f);
        if (maxJitterMs <= 0) {
            return 0;
        }

        // Map the upper bits of the LCG state to a non-negative
        // integer in [0, maxJitterMs]. Using a power-of-two divisor
        // here keeps the math branch-free and bit-reproducible.
        const uint32_t r = (jbmLcg >> 8) & 0xFFFFu;          // [0, 65535]
        return (int)((uint64_t)r * (uint64_t)(uint32_t)maxJitterMs / 65535u);
    }

    void EVSCodecJbm::jbmEnqueue(const unsigned char* au,
                                 int auBits,
                                 uint16_t seq,
                                 uint32_t rtpTsMs) {
        if (!au || auBits <= 0) {
            return;
        }

        JbmPacket p;
        p.auBits  = auBits;
        p.seq     = seq;
        p.rtpTsMs = rtpTsMs;

        // Round the bit count up to whole bytes; EvsRXlib's
        // EVS_RX_FeedFrame copies ceil(au_bits_count / 8) bytes from
        // the AU buffer.
        const size_t bytes = (size_t)((auBits + 7) / 8);
        p.au.assign(au, au + bytes);

        // Stamp the deterministic arrival time.
        p.recvMs = rtpTsMs + (uint32_t)jbmArrivalOffsetMs();

        jbmQueue.push_back(std::move(p));
        jbmTrim();
    }

    void EVSCodecJbm::jbmDrain(uint32_t sysMs) {
        if (!rx) {
            // No receiver; drop everything so a stale queue can't
            // grow without bound across modes.
            jbmQueue.clear();
            return;
        }

        // Walk oldest-first and feed every packet whose arrival time
        // is now in the past. Errors from individual feeds are
        // ignored: the JBM will conceal any slot that didn't make it
        // in, which is exactly the behaviour we want for late / lost
        // packets.
        size_t i = 0;
        for (; i < jbmQueue.size(); ++i) {
            if (jbmQueue[i].recvMs > sysMs) {
                break;
            }
            const JbmPacket& p = jbmQueue[i];
            (void)evs_rx_jbm_feed_frame(rx,
                                        p.au.empty() ? nullptr : p.au.data(),
                                        p.auBits,
                                        p.seq,
                                        (unsigned long)p.rtpTsMs,
                                        (unsigned int)p.recvMs);
        }
        if (i > 0) {
            jbmQueue.erase(jbmQueue.begin(), jbmQueue.begin() + (std::ptrdiff_t)i);
        }
    }

    void EVSCodecJbm::jbmTrim() {
        // Drop the oldest packets if we're at or above the cap.
        // Insertion order is ascending by rtpTsMs (recvMs >= rtpTsMs)
        // because we only append, so the head of the vector is
        // always the oldest.
        if (jbmQueue.size() >= (size_t)kMaxQueueSize) {
            const size_t excess = jbmQueue.size() - (size_t)kMaxQueueSize + 1;
            jbmQueue.erase(jbmQueue.begin(), jbmQueue.begin() + (std::ptrdiff_t)excess);
        }
    }

    void EVSCodecJbm::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = getFrameSize();

        // Deterministic 20 ms timeline, advanced exactly once per
        // input frame (not per successful encode) so the JBM sees a
        // gap-free RTP timestamp stream even when a packet is dropped.
        const uint32_t t_ms = frameIndex * 20u;
        const uint16_t seq  = (uint16_t)rtpSeq;

        // Always advance the timeline + sequence number, even on
        // packet loss: the JBM/decoder needs to know which slot was
        // missed in order to conceal it deterministically.
        frameIndex++;
        rtpSeq++;

        // ----- Drain queue (deliver past-due arrivals first) -----
        // Pull every queued packet whose recvMs is <= the current
        // system time so the JBM gets any late arrivals before we
        // push the next fresh AU on top.
        jbmDrain(t_ms);

        // ----- Encode side -----
        // Only encode and enqueue a new AU when the caller's network
        // model did not drop this frame. If we have no encoder handle
        // (e.g. both _ex and legacy create failed) we fall through to
        // the "no feed" path and let the JBM conceal.
        int nb_bits = 0;
        bool have_au = false;
        if (!packetLost && enc && rx) {
            int used = 0;
            if (evs_enc_process(enc, in, fs, bitstream.data(),
                                (int)bitstream.size(), &used) == EVS_OK) {
                nb_bits = evs_rx_jbm_g192_to_compact_au(
                    bitstream.data(), used,
                    compactAu.data(), (int)compactAu.size());
                if (nb_bits > 0) {
                    have_au = true;
                }
            }
        }

        // ----- JBM feed via the deterministic arrival queue -----
        if (have_au) {
            // Enqueue this AU with a deterministic recvMs offset.
            // We do NOT feed the JBM directly any more: jbmDrain() at
            // the top of the next processFrame() call will pick this
            // packet up the moment its recvMs is in the past.
            jbmEnqueue(compactAu.data(), nb_bits, seq, t_ms);
        }

        // ----- JBM pull (always one 20 ms output frame) -----
        // We always pull, even on packet loss, so the contract
        // (one input frame in, one output frame out) is preserved.
        // The JBM conceals the missing slot internally when no AU
        // was fed; if rx is missing or errors out, we fall back to
        // the waveform concealer.
        short pcmBuf[4096];
        if ((int)(sizeof(pcmBuf) / sizeof(pcmBuf[0])) < fs) {
            // Defensive: should never happen for the supported
            // sample rates (max 48 kHz -> 960 samples/frame).
            fallbackPLC.conceal(out, fs);
            return;
        }

        if (rx) {
            int n = 0;
            if (evs_rx_jbm_get_samples(rx, pcmBuf, fs,
                                       (unsigned int)t_ms, &n) == EVS_OK
                && n == fs) {
                std::memcpy(out, pcmBuf, fs * sizeof(int16_t));
                fallbackPLC.storeGoodFrame(out, fs);
                // After a successful pull, read the JBM's latest
                // channel-aware FEC estimate and push it into the
                // encoder for the next frame (13.2 kbps / >= 16 kHz
                // only; rfActive is latched at ctor time).
                if (rfActive && rx && enc) {
                    int off = 0, hi = 0;
                    if (evs_rx_jbm_get_fec_offset(rx, &off, &hi) == EVS_OK) {
                        if (off != lastAppliedFecOffset || hi != lastAppliedFecHi) {
                            const int encOff = (off == 1) ? 0 : off;
                            if (encOff == 0 || encOff == 2 || encOff == 3 || encOff == 5 || encOff == 7) {
                                evs_enc_set_rf(enc, 1, encOff, hi);
                                lastAppliedFecOffset = off;
                                lastAppliedFecHi     = hi;
                            }
                        }
                    }
                }
                return;
            }
        }

        // Last-resort concealment: no JBM output (missing handle or
        // pull failure). On packet loss we conceal; otherwise we
        // pass the input through to avoid an audible click.
        if (packetLost) {
            fallbackPLC.conceal(out, fs);
        } else {
            std::memcpy(out, in, fs * sizeof(int16_t));
            fallbackPLC.storeGoodFrame(out, fs);
        }
    }
#else // TELEPHONY_DISTRIBUTION_BUILD
    // Distribution-build stub: keeps the symbol table clean while the
    // experimental mode is hidden by distributionSafeMode. The stub
    // mirrors EVSCodec's distribution path: no external API calls,
    // pass-through behaviour, no JBM handle, no encoder handle.
    EVSCodecJbm::EVSCodecJbm(int sampleRate, int, EVS_Bandwidth)
        : sampleRate(sampleRate), bitrateBps(0), maxBw(EVS_SWB),
          enc(nullptr), rx(nullptr), frameIndex(0), rtpSeq(0),
          jbmQueue(), jbmLcg(0x9E3779B9u),
          cfgPacketLossRate(0.0f), cfgNetworkDegradation(0.0f),
          rfActive(false), lastAppliedFecOffset(-1), lastAppliedFecHi(-1) {
        fallbackPLC.reset(getFrameSize());
        jbmQueue.reserve(kMaxQueueSize);
    }
    EVSCodecJbm::~EVSCodecJbm() {}
    void EVSCodecJbm::reset() {
        fallbackPLC.reset(getFrameSize());
        frameIndex = 0;
        rtpSeq = 0;
        // Keep the queue + LCG bookkeeping consistent with the real
        // path so a stale currentMode can never blow up.
        jbmQueue.clear();
        jbmLcg = 0x9E3779B9u;
        // cfg* are preserved across reset() in the real path; mirror
        // that here even though the distribution stub has no encoder
        // to push them into.
    }
    void EVSCodecJbm::configureNetwork(float packetLossRate, float networkDegradation) {
        // Distribution stub: clamp + store only, no queue behaviour.
        cfgPacketLossRate     = std::clamp(packetLossRate, 0.0f, 0.95f);
        cfgNetworkDegradation = std::clamp(networkDegradation, 0.0f, 1.0f);
    }
    void EVSCodecJbm::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = getFrameSize();
        // Advance the timeline so the JBM-side state stays consistent
        // if this stub is ever reached (e.g. via a stale currentMode).
        frameIndex++;
        rtpSeq++;
        if (packetLost) {
            fallbackPLC.conceal(out, fs);
        } else {
            std::memcpy(out, in, fs * sizeof(int16_t));
            fallbackPLC.storeGoodFrame(out, fs);
        }
    }
    // Stub helpers: declared in the header so both builds link, never
    // called from the distribution path (the stub processFrame above
    // does not touch the queue). Defined out-of-line so the symbols
    // exist with no-op semantics.
    int  EVSCodecJbm::jbmArrivalOffsetMs() { return 0; }
    void EVSCodecJbm::jbmEnqueue(const unsigned char*, int, uint16_t, uint32_t) { jbmTrim(); }
    void EVSCodecJbm::jbmDrain(uint32_t) { jbmQueue.clear(); }
    void EVSCodecJbm::jbmTrim() {
        if (jbmQueue.size() >= (size_t)kMaxQueueSize) {
            const size_t excess = jbmQueue.size() - (size_t)kMaxQueueSize + 1;
            jbmQueue.erase(jbmQueue.begin(), jbmQueue.begin() + (std::ptrdiff_t)excess);
        }
    }
#endif // !TELEPHONY_DISTRIBUTION_BUILD
#endif // TELEPHONY_USE_EVS_JBM

} // namespace TelephonyDSP
