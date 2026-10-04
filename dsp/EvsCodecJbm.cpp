#include <cstring>
#include "dsp/EvsCodecJbm.h"
#include "dsp/EvsCodec.h"
#include <array>
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
    EVSCodecJbm::EVSCodecJbm(int sampleRate, int bitrateBps, EVS_Bandwidth maxBw,
                             int dtxSidInterval, bool scVbrEnabled)
        : sampleRate(sampleRate)
        , bitrateBps(bitrateBps)
        , maxBw(maxBw)
        , dtxSidInterval(dtxSidInterval)
        , scVbrEnabled(scVbrEnabled)
        , enc(nullptr)
        , rx(nullptr)
        , frameIndex(0)
        , rtpSeq(0)
        , jbmQueue()
        , jbmLcg(0x9E3779B9u)            // golden-ratio LCG seed; same as OpusCodec
        , cfgPacketLossRate(0.0f)
        , cfgNetworkDegradation(0.0f)
    {
        this->dtxSidInterval = dtxSidInterval >= 3 && dtxSidInterval <= 100 ? dtxSidInterval : 0;
        fixedBitrateBps = bitrateBps == 5900 ? 13200 : bitrateBps; fixedMaxBw = maxBw;
        jbmQueue.reserve(kMaxQueueSize);
        bitstream.resize(evs_max_bitstream_bytes(sampleRate));
        compactAu.assign(320, 0);
        pendingEncoderPcm.reserve((size_t)getFrameSize() * 2 + 4);
        encodedBatch.reserve(2); arrivedBatch.reserve(kMaxQueueSize);
        reset();
    }

    EVSCodecJbm::~EVSCodecJbm() {
        if (enc) { evs_enc_destroy(enc); enc = nullptr; }
        if (rx)  { evs_rx_jbm_destroy(rx); rx = nullptr; }
    }

    void EVSCodecJbm::reset() {
        resetImpairments(); activeBw = pendingBw = maxBw; bandwidthHold = 0;
        receiverClockMs = lastSourcePlayoutMs = 0; havePlayoutClock = false;
        sourceSampleAccumulator = 0; previousSourceSample = 0; havePreviousSourceSample = false;
        generatedSourceSamples = encodedFrameCount = 0;
        pendingEncoderPcm.clear(); encodedBatch.clear(); arrivedBatch.clear();
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
        opts.dtx_enable       = dtxEnabled ? 1 : 0;
        opts.dtx_sid_interval = dtxSidInterval;
        opts.rf_enable        = (bitrateBps == EVS_BR_13200 && sampleRate >= 16000) ? 1 : 0;
        opts.rf_fec_offset   = 0;     // use FEC_OFFSET default (3)
        opts.rf_fec_hi       = 1;
        opts.sc_vbr_enable    = scVbrEnabled ? 1 : 0;
        int rate = bitrateBps; auto bandwidth = maxBw;
        if (evs_enc_normalize_config(sampleRate, &rate, &bandwidth, &opts) == EVS_OK) {
            bitrateBps = rate; maxBw = activeBw = pendingBw = bandwidth;
            scVbrEnabled = opts.sc_vbr_enable != 0;
            enc = evs_enc_create_ex(sampleRate, rate, bandwidth, &opts);
        }
        rfActive = opts.rf_enable != 0;
        lastAppliedFecOffset = lastAppliedFecHi = -1;

        // Tear down + recreate the JBM. EVS_RX_Close destroys the
        // embedded Decoder_State contents; the adapter frees its
        // own Decoder_State allocation, so this is a full reset.
        if (rx) { evs_rx_jbm_destroy(rx); rx = nullptr; }
        rx = evs_rx_jbm_create_ex(sampleRate, bitrateBps, safetyMarginMs);
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

    void EVSCodecJbm::setDtxSidInterval(int interval) {
        // Same normalization as the ctor: 0 (variable) or 3..100 (fixed
        // frames). Anything else collapses to 0 so evs_enc_create_ex never
        // rejects the new configuration.
        const int normalized = (interval == 0) ? 0
                              : ((interval >= 3 && interval <= 100) ? interval : 0);
        if (normalized == dtxSidInterval) {
            return;
        }
        dtxSidInterval = normalized;
        applyConfiguration(bitrateBps, activeBw, false);
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
        std::stable_sort(jbmQueue.begin(), jbmQueue.end(), [](const JbmPacket& a, const JbmPacket& b) { return a.recvMs < b.recvMs; });
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

        // Receiver deadlines run at one 20 ms sound-card slot per call.
        // A separate source resampling clock creates 0/1/2 encoded packets,
        // so clock mismatch changes queue occupancy and reaches the real APA.
        const uint32_t t_ms = frameIndex++ * 20u;
        const double sourceRatio = 1.0 / (1.0 + clockDriftPpm * 1e-6);
        estimateBandwidth(in);
        for (int i = 0; i < fs; ++i) {
            const int16_t current = dtxEnabled && hasVoiceActivity && !voiceActive ? 0 : in[i];
            if (!havePreviousSourceSample) { previousSourceSample = current; havePreviousSourceSample = true; }
            sourceSampleAccumulator += sourceRatio;
            while (sourceSampleAccumulator >= 1.0) {
                const double fraction = std::clamp(1.0 - (sourceSampleAccumulator - 1.0) / sourceRatio, 0.0, 1.0);
                const double value = previousSourceSample + fraction * (current - (double)previousSourceSample);
                pendingEncoderPcm.push_back((int16_t)std::clamp(std::lround(value), -32768l, 32767l));
                ++generatedSourceSamples; sourceSampleAccumulator -= 1.0;
            }
            previousSourceSample = current;
        }
        encodedBatch.clear();
        size_t consumed = 0;
        while (pendingEncoderPcm.size() - consumed >= (size_t)fs) {
            int used = 0, bits = 0;
            const bool encoded = enc && evs_enc_process(enc, pendingEncoderPcm.data() + consumed, fs,
                bitstream.data(), (int)bitstream.size(), &used) == EVS_OK;
            if (enc) evs_enc_get_last_frame_info(enc, &bits, nullptr, nullptr);
            if (encoded) corruptG192(bitstream.data(), (size_t)used);
            CodecEncodedPacket packet;
            packet.info = {CodecPacketFormat::EVSG192, sampleRate, fs, bits == 0 || bits == 48};
            packet.sequence = encodedFrameCount++;
            packet.sourceTimestampMs = (double)packet.sequence * 20.0;
            packet.lost = packetLost || !encoded;
            if (encoded) packet.payload.assign(bitstream.begin(), bitstream.begin() + used);
            encodedBatch.push_back(std::move(packet));
            ++rtpSeq; consumed += (size_t)fs;
        }
        if (consumed) pendingEncoderPcm.erase(pendingEncoderPcm.begin(), pendingEncoderPcm.begin() + (std::ptrdiff_t)consumed);

        if (packetTransport && packetTransport->supportsBatchIngress()) {
            packetTransport->exchangeBatch(encodedBatch, t_ms, arrivedBatch);
            for (const auto& received : arrivedBatch) {
                const int bits = evs_rx_jbm_g192_to_compact_au(received.payload.data(), (int)received.payload.size(),
                    compactAu.data(), (int)compactAu.size());
                if (rx && bits > 0 && received.hasTiming)
                    evs_rx_jbm_feed_frame(rx, compactAu.data(), bits, (uint16_t)received.sequence,
                        (uint32_t)std::max(0.0, std::round(received.sourceTimestampMs)),
                        (uint32_t)std::max(0.0, std::round(received.arrivalTimeMs)));
            }
        } else {
            // Direct/capture transports retain their synchronous compatibility
            // path. Without an external transport the encoded queue is clocked
            // solely by receiver time, independent of source RTP timestamps.
            jbmDrain(t_ms);
            for (const auto& packet : encodedBatch) {
                const bool received = exchangePacket(CodecPacketFormat::EVSG192, packet.payload.data(),
                    packet.payload.size(), packet.lost, packet.info.dtx);
                if (!received) continue;
                const int bits = evs_rx_jbm_g192_to_compact_au(playout.payload.data(), (int)playout.payload.size(),
                    compactAu.data(), (int)compactAu.size());
                if (bits <= 0 || !rx) continue;
                if (packetTransport) {
                    const uint16_t originalSeq = playout.hasTiming ? (uint16_t)playout.sequence : (uint16_t)packet.sequence;
                    const uint32_t sourceTs = playout.hasTiming ? (uint32_t)std::max(0.0, std::round(playout.sourceTimestampMs)) : (uint32_t)packet.sourceTimestampMs;
                    const uint32_t arrival = playout.hasTiming ? (uint32_t)std::max(0.0, std::round(playout.arrivalTimeMs)) : t_ms;
                    evs_rx_jbm_feed_frame(rx, compactAu.data(), bits, originalSeq, sourceTs, arrival);
                } else {
                    JbmPacket queued; queued.auBits = bits; queued.seq = (uint16_t)packet.sequence;
                    queued.rtpTsMs = (uint32_t)packet.sourceTimestampMs;
                    queued.recvMs = t_ms + (uint32_t)jbmArrivalOffsetMs();
                    queued.au.assign(compactAu.begin(), compactAu.begin() + (bits + 7) / 8);
                    jbmQueue.push_back(std::move(queued)); jbmTrim();
                }
            }
        }
        receiverClockMs = (double)frameIndex * 20.0;
        lastSourcePlayoutMs = generatedSourceSamples * 1000.0 / sampleRate;
        const uint32_t receiverMs = t_ms;

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
                                       (unsigned int)receiverMs, &n) == EVS_OK
                && n == fs) {
                std::memcpy(out, pcmBuf, fs * sizeof(int16_t));
                applyDtxOutput(out, evs_rx_jbm_in_dtx(rx) != 0);
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
    void EVSCodecJbm::setSafetyMarginMs(int milliseconds) {
        const int normalized = std::clamp(milliseconds, 0, 1000);
        if (normalized == safetyMarginMs) return;
        safetyMarginMs = normalized; reset();
    }
    bool EVSCodecJbm::isWarmingUp() const { return !evs_rx_jbm_has_started(rx); }
    void EVSCodecJbm::setClockDriftPpm(double ppm) {
        clockDriftPpm = std::isfinite(ppm) ? std::clamp(ppm, -50.0, 50.0) : 0.0;
    }
    bool EVSCodecJbm::applyConfiguration(int bitrate, EVS_Bandwidth bw, bool storeCeiling) {
        const int requestedBitrate = bitrate; const auto requestedBw = bw;
        EVS_EncOptions opts; evs_enc_options_init(&opts);
        opts.dtx_enable = dtxEnabled; opts.dtx_sid_interval = dtxSidInterval;
        opts.sc_vbr_enable = scVbrEnabled;
        opts.rf_enable = bitrate == 13200 && sampleRate >= 16000 && bw != EVS_NB;
        if (evs_enc_normalize_config(sampleRate, &bitrate, &bw, &opts) != EVS_OK || !enc ||
            evs_enc_reconfigure(enc, bitrate, bw, &opts) != EVS_OK) return false;
        if (storeCeiling && opts.sc_vbr_enable && requestedBitrate != 5900) {
            auto fixedOpts = opts; fixedOpts.sc_vbr_enable = 0;
            int fixedRate = requestedBitrate; auto fixedBw = requestedBw;
            if (evs_enc_normalize_config(sampleRate, &fixedRate, &fixedBw, &fixedOpts) == EVS_OK) {
                fixedBitrateBps = fixedRate; fixedMaxBw = fixedBw;
            }
        }
        bitrateBps = bitrate; activeBw = bw; if (storeCeiling) maxBw = bw;
        if (storeCeiling && !scVbrEnabled) { fixedBitrateBps = bitrate; fixedMaxBw = bw; }
        rfActive = opts.rf_enable != 0; lastAppliedFecOffset = lastAppliedFecHi = -1;
        return true;
    }
    bool EVSCodecJbm::reconfigure(int bitrate, EVS_Bandwidth bw) { return applyConfiguration(bitrate, bw, true); }
    void EVSCodecJbm::configureDtx(bool enabled, bool pureSilence) {
        const bool changed = enabled != dtxEnabled;
        ICodec::configureDtx(enabled, pureSilence);
        if (changed) applyConfiguration(bitrateBps, activeBw, false);
    }
    void EVSCodecJbm::setScVbrEnabled(bool enabled) {
        if (enabled == scVbrEnabled) return;
        const bool before = scVbrEnabled;
        if (enabled) { fixedBitrateBps = bitrateBps; fixedMaxBw = maxBw; }
        scVbrEnabled = enabled;
        if (!reconfigure(enabled ? 5900 : fixedBitrateBps, enabled ? maxBw : fixedMaxBw)) scVbrEnabled = before;
    }
    void EVSCodecJbm::setAutoBandwidth(bool enabled) {
        if (enabled == autoBandwidth) return;
        autoBandwidth = enabled; bandwidthHold = 0;
        if (!enabled) applyConfiguration(bitrateBps, maxBw, false);
    }
    void EVSCodecJbm::estimateBandwidth(const int16_t* input) {
        if (!autoBandwidth) return;
        auto candidate = estimateEvsInputBandwidth(input, getFrameSize(), sampleRate, maxBw, activeBw);
        if (candidate == EVS_NB && bitrateBps > 24400) candidate = EVS_WB;
        if (candidate != pendingBw) { pendingBw = candidate; bandwidthHold = 1; } else ++bandwidthHold;
        if (pendingBw != activeBw && bandwidthHold >= (pendingBw > activeBw ? 2 : 8))
            applyConfiguration(bitrateBps, pendingBw, false);
    }
#else // TELEPHONY_DISTRIBUTION_BUILD
    // Distribution-build stub: keeps the symbol table clean while the
    // experimental mode is hidden by distributionSafeMode. The stub
    // mirrors EVSCodec's distribution path: no external API calls,
    // pass-through behaviour, no JBM handle, no encoder handle.
    EVSCodecJbm::EVSCodecJbm(int sampleRate, int, EVS_Bandwidth, int dtxSidInterval, bool scVbrEnabled)
        : sampleRate(sampleRate), bitrateBps(0), maxBw(EVS_SWB),
          dtxSidInterval(0), scVbrEnabled(scVbrEnabled),
          enc(nullptr), rx(nullptr), frameIndex(0), rtpSeq(0),
          jbmQueue(), jbmLcg(0x9E3779B9u),
          cfgPacketLossRate(0.0f), cfgNetworkDegradation(0.0f),
          rfActive(false), lastAppliedFecOffset(-1), lastAppliedFecHi(-1) {
        // Same normalization as the real path: 0 or 3..100 only.
        dtxSidInterval = (dtxSidInterval == 0) ? 0
                       : ((dtxSidInterval >= 3 && dtxSidInterval <= 100) ? dtxSidInterval : 0);
        fallbackPLC.reset(getFrameSize());
        jbmQueue.reserve(kMaxQueueSize);
    }
    EVSCodecJbm::~EVSCodecJbm() {}
    void EVSCodecJbm::reset() {
        resetImpairments(); activeBw = pendingBw = maxBw; bandwidthHold = 0;
        receiverClockMs = lastSourcePlayoutMs = 0; havePlayoutClock = false;
        sourceSampleAccumulator = 0; previousSourceSample = 0; havePreviousSourceSample = false;
        generatedSourceSamples = encodedFrameCount = 0;
        pendingEncoderPcm.clear(); encodedBatch.clear(); arrivedBatch.clear();
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
    void EVSCodecJbm::setDtxSidInterval(int interval) {
        // Distribution stub: no encoder to push into, but keep the
        // stored value in sync with the real path so a build-flipped
        // binary state stays consistent.
        dtxSidInterval = (interval == 0) ? 0
                       : ((interval >= 3 && interval <= 100) ? interval : 0);
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
    void EVSCodecJbm::setSafetyMarginMs(int milliseconds) { safetyMarginMs = std::clamp(milliseconds, 0, 1000); }
    bool EVSCodecJbm::isWarmingUp() const { return false; }
    void EVSCodecJbm::setClockDriftPpm(double ppm) { clockDriftPpm = std::isfinite(ppm) ? std::clamp(ppm, -50.0, 50.0) : 0.0; }
    bool EVSCodecJbm::applyConfiguration(int, EVS_Bandwidth, bool) { return false; }
    bool EVSCodecJbm::reconfigure(int, EVS_Bandwidth) { return false; }
    void EVSCodecJbm::configureDtx(bool enabled, bool pureSilence) { ICodec::configureDtx(enabled, pureSilence); }
    void EVSCodecJbm::setScVbrEnabled(bool enabled) { scVbrEnabled = enabled; }
    void EVSCodecJbm::setAutoBandwidth(bool enabled) { autoBandwidth = enabled; }
    void EVSCodecJbm::estimateBandwidth(const int16_t*) {}
#endif // !TELEPHONY_DISTRIBUTION_BUILD
#endif // TELEPHONY_USE_EVS_JBM

} // namespace TelephonyDSP
