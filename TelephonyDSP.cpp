#define NOMINMAX
#include "TelephonyDSP.h"
#include <cstring>
#include <algorithm>
#include <cmath>

extern "C" {
#include "gsm.h"
}
#ifndef TELEPHONY_DISTRIBUTION_BUILD
#include "interf_enc.h" // AMR-NB
#include "interf_dec.h"
#include "enc_if.h" // AMR-WB (vo-amrwbenc)
#include "dec_if.h" // AMR-WB (opencore-amrwb)
#endif

#if TELEPHONY_EXPERIMENTAL_NETWORK
extern "C" {
#include <opus.h>
#include "speex/speex_preprocess.h"
#include "speex/speex_jitter.h"
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace TelephonyDSP {

#ifdef TELEPHONY_DISTRIBUTION_BUILD
 static EraMode distributionSafeMode(EraMode mode) {
 // Defense-in-depth: experimental codecs (EVS_NATIVE, OPUS_VOIP, and - if
 // it is exposed in this build - EVS_JBM) are disabled in distribution
 // builds, but their enum entries remain reachable. Map them to the
 // safe EVS_LIKE profile.
#if TELEPHONY_USE_EVS_JBM
 return (mode == EraMode::EVS_NATIVE
         || mode == EraMode::OPUS_VOIP
         || mode == EraMode::EVS_JBM)
        ? EraMode::EVS_LIKE
        : mode;
#else
 return (mode == EraMode::EVS_NATIVE || mode == EraMode::OPUS_VOIP)
 ? EraMode::EVS_LIKE
 : mode;
#endif
 }
#else
    static EraMode distributionSafeMode(EraMode mode) {
        return mode;
    }
#endif

    static int16_t clampToInt16(float v) {
        return (int16_t)std::clamp(v, -32768.0f, 32767.0f);
    }

    void WaveformConcealer::reset(int frameSize) {
        history.assign((std::max)(frameSize * 4, frameSize), 0);
        attenuation = 1.0f;
        lastPitch = (std::max)(20, frameSize / 2);
    }

    void WaveformConcealer::storeGoodFrame(const int16_t* frame, int frameSize) {
        if (frameSize <= 0) return;
        if (history.size() < (size_t)(frameSize * 4)) {
            reset(frameSize);
        }

        std::memmove(history.data(), history.data() + frameSize,
                     (history.size() - frameSize) * sizeof(int16_t));
        std::memcpy(history.data() + history.size() - frameSize, frame, frameSize * sizeof(int16_t));
        attenuation = 1.0f;
        lastPitch = estimatePitch(frameSize);
    }

    int WaveformConcealer::estimatePitch(int frameSize) const {
        if (frameSize <= 0 || history.size() < (size_t)(frameSize * 2)) {
            return (std::max)(20, frameSize / 2);
        }

        const int histSize = (int)history.size();
        const int current = histSize - frameSize;
        const int minLag = (std::max)(20, frameSize / 8);
        const int maxLag = (std::min)(frameSize * 2, current - 1);
        if (maxLag <= minLag) return (std::max)(20, frameSize / 2);

        float bestScore = -1.0f;
        int bestLag = lastPitch;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            const int ref = current - lag;
            if (ref < 0) break;

            double corr = 0.0;
            double e1 = 1.0;
            double e2 = 1.0;
            for (int i = 0; i < frameSize; ++i) {
                const double a = history[current + i];
                const double b = history[ref + i];
                corr += a * b;
                e1 += a * a;
                e2 += b * b;
            }
            const float score = (float)(corr / std::sqrt(e1 * e2));
            if (score > bestScore) {
                bestScore = score;
                bestLag = lag;
            }
        }

        return bestScore > 0.15f ? bestLag : (std::max)(20, frameSize / 2);
    }

    void WaveformConcealer::conceal(int16_t* out, int frameSize) {
        if (frameSize <= 0) return;
        if (history.size() < (size_t)(frameSize * 2)) {
            reset(frameSize);
        }

        const int histSize = (int)history.size();
        const int pitch = std::clamp(lastPitch, 1, histSize);
        const int start = histSize - pitch;
        std::vector<int16_t> concealed(frameSize);

        for (int i = 0; i < frameSize; ++i) {
            const float intraFrameFade = 1.0f - 0.15f * ((float)i / (float)(std::max)(1, frameSize - 1));
            const int idx = start + (i % pitch);
            concealed[i] = clampToInt16((float)history[idx] * attenuation * intraFrameFade);
        }

        std::memcpy(out, concealed.data(), frameSize * sizeof(int16_t));
        std::memmove(history.data(), history.data() + frameSize,
                     (history.size() - frameSize) * sizeof(int16_t));
        std::memcpy(history.data() + history.size() - frameSize, concealed.data(), frameSize * sizeof(int16_t));
        attenuation = (std::max)(0.05f, attenuation * 0.82f);
    }

    RingBuffer::RingBuffer(size_t size) : mask(0), writePos(0), readPos(0) {
        if (size > 0) resize(size);
    }

    void RingBuffer::resize(size_t size) {
        size_t pot = 1;
        while (pot < size) pot <<= 1;
        buffer.resize(pot, 0.0f);
        mask = pot - 1;
        reset();
    }

    void RingBuffer::reset() {
        writePos = 0;
        readPos = 0;
        std::fill(buffer.begin(), buffer.end(), 0.0f);
    }

    size_t RingBuffer::getReadAvailable() const { return writePos - readPos; }
    size_t RingBuffer::getWriteAvailable() const { return buffer.size() - (writePos - readPos); }

    size_t RingBuffer::write(const float* data, size_t count) {
        size_t available = getWriteAvailable();
        if (count > available) count = available;
        size_t off = writePos & mask;
        size_t n1 = (std::min)(count, buffer.size() - off);
        std::memcpy(buffer.data() + off, data, n1 * sizeof(float));
        if (n1 < count) std::memcpy(buffer.data(), data + n1, (count - n1) * sizeof(float));
        writePos += count;
        return count;
    }

    size_t RingBuffer::read(float* data, size_t count) {
        size_t available = getReadAvailable();
        if (count > available) count = available;
        size_t off = readPos & mask;
        size_t n1 = (std::min)(count, buffer.size() - off);
        std::memcpy(data, buffer.data() + off, n1 * sizeof(float));
        if (n1 < count) std::memcpy(data + n1, buffer.data(), (count - n1) * sizeof(float));
        readPos += count;
        return count;
    }

    size_t RingBuffer::peek(float* data, size_t count) const {
        size_t available = getReadAvailable();
        if (count > available) count = available;
        size_t off = readPos & mask;
        size_t n1 = (std::min)(count, buffer.size() - off);
        std::memcpy(data, buffer.data() + off, n1 * sizeof(float));
        if (n1 < count) std::memcpy(data + n1, buffer.data(), (count - n1) * sizeof(float));
        return count;
    }

    void RingBuffer::skip(size_t count) {
        size_t available = getReadAvailable();
        if (count > available) count = available;
        readPos += count;
    }

    G711Codec::G711Codec(int sr) : sampleRate(sr) {
        plc.reset(getFrameSize());
    }
    void G711Codec::reset() {
        plc.reset(getFrameSize());
    }
    void G711Codec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = getFrameSize();
        if (packetLost) {
            plc.conceal(out, fs);
            return;
        }

        for (int i = 0; i < fs; ++i) {
            unsigned char u = linear2ulaw(in[i]);
            out[i] = (int16_t)ulaw2linear(u);
        }
        plc.storeGoodFrame(out, fs);
    }

    GSMCodec::GSMCodec() { gsmState = gsm_create(); plc.reset(getFrameSize()); }
    GSMCodec::~GSMCodec() { if (gsmState) gsm_destroy((gsm)gsmState); }
    void GSMCodec::reset() { if (gsmState) gsm_destroy((gsm)gsmState); gsmState = gsm_create(); plc.reset(getFrameSize()); }
    void GSMCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            plc.conceal(out, getFrameSize());
            return;
        }

        gsm_byte frame_encoded[33];
        gsm_encode((gsm)gsmState, (gsm_signal*)in, frame_encoded);
        gsm_decode((gsm)gsmState, frame_encoded, (gsm_signal*)out);
        plc.storeGoodFrame(out, getFrameSize());
    }

#ifndef TELEPHONY_DISTRIBUTION_BUILD
    AMRNBCodec::AMRNBCodec(bool dtxEnabled) : dtxEnabled(dtxEnabled), lastSerial(32, 0) {
        encState = Encoder_Interface_init(dtxEnabled ? 1 : 0);
        decState = Decoder_Interface_init();
        fallbackPLC.reset(getFrameSize());
    }
    AMRNBCodec::~AMRNBCodec() { Encoder_Interface_exit(encState); Decoder_Interface_exit(decState); }
    void AMRNBCodec::setDtxEnabled(bool enable) {
        if (dtxEnabled == enable) return;
        // Allocate the replacement encoder first so we don't drop the
        // existing state if init fails; on failure we keep both the old
        // encoder and the previous dtxEnabled value.
        void* newEnc = Encoder_Interface_init(enable ? 1 : 0);
        if (!newEnc) return;
        Encoder_Interface_exit(encState);
        encState = newEnc;
        dtxEnabled = enable;
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRNBCodec::reset() {
        Encoder_Interface_exit(encState);
        Decoder_Interface_exit(decState);
        encState = Encoder_Interface_init(dtxEnabled ? 1 : 0);
        decState = Decoder_Interface_init();
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRNBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            Decoder_Interface_Decode(decState, lastSerial.data(), (short*)out, 1);
            fallbackPLC.storeGoodFrame(out, getFrameSize());
            return;
        }

        Encoder_Interface_Encode(encState, MR122, (short*)in, lastSerial.data(), 0);
        Decoder_Interface_Decode(decState, lastSerial.data(), (short*)out, 0);
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }

    AMRWBCodec::AMRWBCodec(bool dtxEnabled) : dtxEnabled(dtxEnabled), lastSerial(64, 0) {
        encState = E_IF_init();
        decState = D_IF_init();
        fallbackPLC.reset(getFrameSize());
    }
    AMRWBCodec::~AMRWBCodec() { E_IF_exit(encState); D_IF_exit(decState); }
    void AMRWBCodec::setDtxEnabled(bool enable) {
        // AMR-WB's E_IF_init() does not take a DTX flag; the DTX setting
        // is the last (5th) argument of E_IF_encode(). We just remember
        // the new flag here; the next processFrame() picks it up.
        dtxEnabled = enable;
    }
    void AMRWBCodec::reset() {
        // dtxEnabled is preserved: AMR-WB DTX is supplied per E_IF_encode
        // call, not during E_IF_init, so resetting the encoder state does
        // not require re-asking the caller for the DTX preference.
        E_IF_exit(encState);
        D_IF_exit(decState);
        encState = E_IF_init();
        decState = D_IF_init();
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRWBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            D_IF_decode(decState, lastSerial.data(), out, 1);
            fallbackPLC.storeGoodFrame(out, getFrameSize());
            return;
        }

        E_IF_encode(encState, 2, in, lastSerial.data(), dtxEnabled ? 1 : 0); // Mode 2: 12.65 kbit/s
        D_IF_decode(decState, lastSerial.data(), out, 0);
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }
#else
    AMRNBCodec::AMRNBCodec(bool dtxEnabled) : encState(nullptr), decState(nullptr), dtxEnabled(dtxEnabled), lastSerial(32, 0) {
        // Distribution stub: no external API calls. dtxEnabled is kept
        // only to keep the constructor signature identical to the
        // non-distribution build, and isDtxEnabled() reports it back.
        fallbackPLC.reset(getFrameSize());
    }
    AMRNBCodec::~AMRNBCodec() {}
    void AMRNBCodec::setDtxEnabled(bool enable) { dtxEnabled = enable; }
    void AMRNBCodec::reset() { fallbackPLC.reset(getFrameSize()); }
    void AMRNBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            fallbackPLC.conceal(out, getFrameSize());
            return;
        }
        std::memcpy(out, in, 160 * sizeof(int16_t));
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }

    AMRWBCodec::AMRWBCodec(bool dtxEnabled) : encState(nullptr), decState(nullptr), dtxEnabled(dtxEnabled), lastSerial(64, 0) {
        // Distribution stub: no external API calls. dtxEnabled is kept
        // only to keep the constructor signature identical to the
        // non-distribution build, and isDtxEnabled() reports it back.
        fallbackPLC.reset(getFrameSize());
    }
    AMRWBCodec::~AMRWBCodec() {}
    void AMRWBCodec::setDtxEnabled(bool enable) { dtxEnabled = enable; }
    void AMRWBCodec::reset() { fallbackPLC.reset(getFrameSize()); }
    void AMRWBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            fallbackPLC.conceal(out, getFrameSize());
            return;
        }
        std::memcpy(out, in, 320 * sizeof(int16_t));
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }
#endif

    // ---------------------------------------------------------------------------
    // EVSCodec
    // ---------------------------------------------------------------------------
    EVSCodec::EVSCodec(int sampleRate, int bitrateBps, EVS_Bandwidth maxBw)
        : sampleRate(sampleRate)
        , bitrateBps(bitrateBps)
        , maxBw(maxBw)
        , enc(nullptr)
        , dec(nullptr)
    {
        fallbackPLC.reset(getFrameSize());
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        // Non-distribution / personal build: enable the 3GPP EVS internal
        // VAD/DTX/SID/CNG path. Variable SID update interval (0) lets the
        // codec pick the per-frame interval, matching the reference CLI's
        // default behaviour. Channel-aware mode (RF) and SC-VBR stay off:
        // the EVS spec only allows RF at 13.2 kbps with >= 16 kHz input, and
        // JBM/RTP-packet-loss handling remain future work (see README).
        EVS_EncOptions opts;
        evs_enc_options_init(&opts);
        opts.dtx_enable       = 1;
        opts.dtx_sid_interval = 0;     // variable SID (see evs_api.h)
        opts.rf_enable        = 0;
        opts.sc_vbr_enable    = 0;
        enc = evs_enc_create_ex(sampleRate, bitrateBps, maxBw, &opts);
        if (!enc) {
            // Refuse cleanly: if DTX is rejected for some reason (e.g. an
            // unsupported configuration we didn't anticipate) fall back to
            // the legacy options so the codec still encodes.
            enc = evs_enc_create(sampleRate, bitrateBps, maxBw);
        }
        dec = evs_dec_create(sampleRate, bitrateBps);
        bitstream.resize(evs_max_bitstream_bytes(sampleRate));
#endif
    }

    EVSCodec::~EVSCodec() {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        if (enc) evs_enc_destroy(enc);
        if (dec) evs_dec_destroy(dec);
#endif
    }

    void EVSCodec::reset() {
        fallbackPLC.reset(getFrameSize());
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        if (enc) { evs_enc_destroy(enc); }
        if (dec) { evs_dec_destroy(dec); dec = evs_dec_create(sampleRate, bitrateBps); }

        // Re-create the encoder with the same DTX options used in the ctor.
        EVS_EncOptions opts;
        evs_enc_options_init(&opts);
        opts.dtx_enable       = 1;
        opts.dtx_sid_interval = 0;
        opts.rf_enable        = 0;
        opts.sc_vbr_enable    = 0;
        enc = evs_enc_create_ex(sampleRate, bitrateBps, maxBw, &opts);
        if (!enc) {
            enc = evs_enc_create(sampleRate, bitrateBps, maxBw);
        }
#endif
    }

void EVSCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = getFrameSize();
        if (packetLost) {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
            int n = 0;
            if (dec && evs_dec_process_lost(dec, out, &n) == EVS_OK && n == fs) {
                fallbackPLC.storeGoodFrame(out, fs);
                return;
            }
#endif
            fallbackPLC.conceal(out, fs);
            return;
        }

        if (!enc || !dec) {
            std::memcpy(out, in, fs * sizeof(int16_t));
            fallbackPLC.storeGoodFrame(out, fs);
            return;
        }
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        int used = 0;
        if (evs_enc_process(enc, in, fs, bitstream.data(), (int)bitstream.size(), &used) != EVS_OK) {
            // On encode failure, pass input through unchanged
            std::memcpy(out, in, fs * sizeof(int16_t));
            fallbackPLC.storeGoodFrame(out, fs);
            return;
        }
        int n = 0;
        if (evs_dec_process(dec, bitstream.data(), used, out, &n) != EVS_OK) {
            std::memcpy(out, in, fs * sizeof(int16_t));
        }
        fallbackPLC.storeGoodFrame(out, fs);
#endif
    }

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
    {
        fallbackPLC.reset(getFrameSize());

        // Match EVSCodec's DTX options. The JBM does not see DTX/SID
        // frames specially (they are still valid AUs in the JBM's MSB-
        // first compact format), so the encoder's DTX/CNG output is
        // pushed through unchanged.
        EVS_EncOptions opts;
        evs_enc_options_init(&opts);
        opts.dtx_enable       = 1;
        opts.dtx_sid_interval = 0;     // variable SID (see evs_api.h)
        opts.rf_enable        = 0;
        opts.sc_vbr_enable    = 0;
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

        // Tear down + recreate the encoder (with the same DTX options
        // used in the ctor).
        if (enc) { evs_enc_destroy(enc); enc = nullptr; }
        EVS_EncOptions opts;
        evs_enc_options_init(&opts);
        opts.dtx_enable       = 1;
        opts.dtx_sid_interval = 0;
        opts.rf_enable        = 0;
        opts.sc_vbr_enable    = 0;
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

        // ----- Encode side -----
        // Only feed the JBM a new AU when the caller's network model
        // did not drop this frame. If we have no encoder handle (e.g.
        // both _ex and legacy create failed) we fall through to the
        // "no feed" path and let the JBM conceal.
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

        // ----- JBM feed (only when we have a real AU) -----
        if (have_au) {
            // Use the same deterministic, zero-jitter 20 ms clock that the
            // reference voip_client loop and EVSJbmSmoke use: RTP timestamp,
            // receive time, and system playout time are all expressed on the
            // JBM's 1000 Hz (ms) timeline. The JBM still applies its own
            // safety-margin / buffering policy internally; we do not read a
            // real wall clock so offline renders remain deterministic.
            const unsigned int sys_ms = (unsigned int)t_ms;
            const unsigned long  ts_ms = (unsigned long)t_ms;
            if (evs_rx_jbm_feed_frame(rx, compactAu.data(), nb_bits,
                                      seq, ts_ms, sys_ms) != EVS_OK) {
                // Feed failed; the JBM will conceal this slot.
                have_au = false;
            }
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
          enc(nullptr), rx(nullptr), frameIndex(0), rtpSeq(0) {
        fallbackPLC.reset(getFrameSize());
    }
    EVSCodecJbm::~EVSCodecJbm() {}
    void EVSCodecJbm::reset() {
        fallbackPLC.reset(getFrameSize());
        frameIndex = 0;
        rtpSeq = 0;
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
#endif // !TELEPHONY_DISTRIBUTION_BUILD
#endif // TELEPHONY_USE_EVS_JBM

    // ---------------------------------------------------------------------------
    // OpusCodec
    // ---------------------------------------------------------------------------
#if TELEPHONY_EXPERIMENTAL_NETWORK
    OpusCodec::OpusCodec(int sr, int bitrate, int complexity)
        : sampleRate(sr)
        , bitrateBps(bitrate)
        , frameSize(sr / 50) // 20 ms
        , complexity(complexity)
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
            // - DTX is intentionally OFF: Opus's DTX is driven by its own
            //   internal VAD, which doesn't match what SpeexDSPAux reports
            //   (and SpeexDSPAux's VAD is currently disabled). A future
            //   iteration that re-enables SpeexDSPAux VAD can drive DTX
            //   from there.
            opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(0));
            opus_encoder_ctl(enc, OPUS_SET_DTX(0));
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
        // DTX stays off; see OpusCodec ctor comment.
    }

    int OpusCodec::arrivalFrameFor(uint32_t seq) {
        // Linear congruential generator step (Numerical Recipes constants).
        jitterLcg = jitterLcg * 1664525u + 1013904223u;
        // Map the upper bits of the LCG state to a non-negative jitter
        // offset in frames. degradation scales the magnitude so a clean
        // network has jitter 0..1 and a heavily degraded one can drift
        // several frames.
        const float d = std::clamp(cfgNetworkDegradation, 0.0f, 1.0f);
        const float maxJitter = 1.0f + d * 4.0f; // up to ~5 frames
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
                // arrivalFrame is computed against the packet's intended
                // playback index (which equals p.seq for a steady, 1-in
                // 1-out transport).
                p.arrivalFrame = (int)(p.seq + basePlaybackDelay());
                // Add a small deterministic jitter offset using the LCG.
                // Subtract basePlaybackDelay here so the caller-visible
                // arrivalFrame stays in terms of playbackFrame index.
                int jitter = arrivalFrameFor(p.seq);
                p.arrivalFrame = (int)p.seq + jitter;
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
            // Walk the queue (it is sorted ascending by seq / arrival) and
            // try, in order: exact match -> FEC of next available packet
            // -> PLC if nothing else.
            //
            // Find the first packet whose arrivalFrame <= targetFrame.
            // (queue is sorted by insertion order which matches arrivalFrame
            // order because arrivalFrame grows with seq and jitter; in the
            // rare case of jitter collapse the earliest-arrival packet
            // wins.)
            size_t idx = 0;
            for (; idx < queue.size(); ++idx) {
                if (queue[idx].arrivalFrame <= targetFrame) break;
            }

            if (idx < queue.size()) {
                // The packet for this playback frame has arrived.
                int n = opus_decode(dec, queue[idx].data.data(),
                                    (opus_int32)queue[idx].data.size(),
                                    out, fs, 0);
                if (n == fs) {
                    decoded = true;
                }
                // Drop the consumed packet. In the jitter-collapse case
                // there could be additional packets with arrivalFrame <=
                // targetFrame still in the queue; those represent future
                // playback frames that arrived early. We leave them in
                // place - they will be picked up at their target playback
                // frame. The queue cap (kMaxQueueSize) keeps growth in
                // check even if collapse happens repeatedly.
                queue.erase(queue.begin() + (std::ptrdiff_t)idx);
            } else if (!queue.empty()) {
                // Target packet is missing but the next one is in flight;
                // try in-band FEC recovery using decode_fec=1. The "next"
                // packet is the one with the smallest seq, which is
                // queue.front() because we insert in seq order.
                int n = opus_decode(dec, queue.front().data.data(),
                                    (opus_int32)queue.front().data.size(),
                                    out, fs, 1);
                if (n == fs) {
                    decoded = true;
                }
                // Whether or not FEC succeeded, drop the consumed packet
                // so the queue doesn't grow forever.
                queue.erase(queue.begin());
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
    OpusCodec::OpusCodec(int, int, int) : sampleRate(0), bitrateBps(0), frameSize(0), complexity(0), encoder(nullptr), decoder(nullptr) {
        fallbackPLC.reset(0);
    }
    OpusCodec::~OpusCodec() {}
    void OpusCodec::reset() { fallbackPLC.reset(frameSize); }
    void OpusCodec::configureNetwork(float, float) {}
    void OpusCodec::processFrame(const int16_t* in, int16_t* out, bool) {
        const int fs = frameSize;
        std::memcpy(out, in, fs * sizeof(int16_t));
    }
#endif

    // ---------------------------------------------------------------------------
    // SpeexDSPAux
    // ---------------------------------------------------------------------------
#if TELEPHONY_EXPERIMENTAL_NETWORK
    SpeexDSPAux::SpeexDSPAux()
 : state(nullptr), sampleRate(0), frameSize(0), configured(false), vadEnabled(false) {}

 SpeexDSPAux::~SpeexDSPAux() {
 if (state) speex_preprocess_state_destroy(static_cast<SpeexPreprocessState*>(state));
 state = nullptr;
 }

 void SpeexDSPAux::configure(int sr, int fs) {
 sampleRate = sr;
 frameSize = fs;
 if (state) {
 speex_preprocess_state_destroy(static_cast<SpeexPreprocessState*>(state));
 state = nullptr;
 }
 SpeexPreprocessState* s = speex_preprocess_state_init(frameSize, sampleRate);
 if (!s) {
 configured = false;
 vadEnabled = false;
 return;
 }
 int denoise =1;
 speex_preprocess_ctl(s, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
 // AGC intentionally disabled: SPEEX_PREPROCESS_SET_AGC_LEVEL takes a
 // float (not an int level). Conservative behavior keeps denoise only;
 // callers can opt-in to AGC later via a float-based API.
 //
 // VAD intentionally disabled: SpeexDSP's VAD is a placeholder
 // ("warning: The VAD has been replaced by a hack pending a complete
 // rewrite") and SpeexDSPAux is currently a foundation/placeholder
 // that does not drive audio behavior. Re-enable here (and plumb the
 // probability into the audio path) once a proper VAD story lands.
 // When re-enabling, flip vadEnabled = true here as well so
 // getSpeechProbability() / lastFrameIsSpeech() start returning
 // Speex-reported values instead of the "unknown" sentinel.
 // speex_preprocess_ctl(s, SPEEX_PREPROCESS_SET_VAD, &vad);
 vadEnabled = false;
 state = s;
 configured = true;
 }

 void SpeexDSPAux::reset() {
 if (!state || !configured) return;
 if (sampleRate >0 && frameSize >0) {
 // SPEEX_PREPROCESS_RESET_STATE doesn't exist in this SpeexDSP
 // build; recreate the state to flush internal buffers.
 configure(sampleRate, frameSize);
 }
 }

 float SpeexDSPAux::getSpeechProbability() const {
 if (!state || !configured) return -1.0f;
 // VAD is disabled by design (see configure()): SPEEX_PREPROCESS_GET_PROB
 // would return whatever the underlying placeholder leaves behind
 // (often 0 or stale), which is not a meaningful speech probability.
 // Return the "-1 = unknown" sentinel so callers can distinguish
 // "no signal" from "definitely not speech".
 if (!vadEnabled) return -1.0f;
 // SPEEX_PREPROCESS_GET_PROB returns speech probability as spx_int32_t
 // (percent, [0,100]). SPEEX_PREPROCESS_GET_PSD is the power spectrum,
 // not speech probability, so it can't be returned as a probability.
 spx_int32_t prob = -1;
 speex_preprocess_ctl(static_cast<SpeexPreprocessState*>(state), SPEEX_PREPROCESS_GET_PROB, &prob);
 if (prob <0) return -1.0f;
 return static_cast<float>(prob) /100.0f;
 }

 bool SpeexDSPAux::lastFrameIsSpeech() const {
 if (!state || !configured) return false;
 // VAD is disabled by design (see configure()); treat the result as
 // "unknown" rather than reporting a false negative.
 if (!vadEnabled) return false;
 int vad =0;
 speex_preprocess_ctl(static_cast<SpeexPreprocessState*>(state), SPEEX_PREPROCESS_GET_VAD, &vad);
 return vad !=0;
 }

 void SpeexDSPAux::runPreprocess(int16_t* frame) {
 if (!state || !configured || !frame) return;
 speex_preprocess_run(static_cast<SpeexPreprocessState*>(state), (spx_int16_t*)frame);
 }
#else
 SpeexDSPAux::SpeexDSPAux() : state(nullptr), sampleRate(0), frameSize(0), configured(false), vadEnabled(false) {}
 SpeexDSPAux::~SpeexDSPAux() {}
 void SpeexDSPAux::configure(int, int) { configured = false; vadEnabled = false; }
 void SpeexDSPAux::reset() {}
 float SpeexDSPAux::getSpeechProbability() const { return -1.0f; }
 bool SpeexDSPAux::lastFrameIsSpeech() const { return false; }
 void SpeexDSPAux::runPreprocess(int16_t*) {}
#endif

    ChannelProcessor::ChannelProcessor(double hostSR)
        : hostSampleRate(hostSR), currentMode(EraMode::Bypass),
          paramArtifactsEnabled(false), paramArtifactAmount(0.0f),
          paramPacketLossRate(0.0f), paramNetworkDegradation(0.0f),
          packetLossSeed(0x12345678u), packetLossBurstFrames(0),
          evsSampleRate(32000), evsBitrateBps(EVS_BR_13200), evsMaxBandwidth(EVS_SWB),
          downMaxInLen(0), upMaxInLen(0)
    {
        size_t bigSize = 131072;
        ringCodecIn.resize(bigSize);
        ringCodecOut.resize(bigSize);
        outputBuffer.resize(bigSize);
        prepareInternalBuffers(4096);
        setMode(EraMode::Bypass); // Initialize logic
    }

    ChannelProcessor::~ChannelProcessor() {}

    void ChannelProcessor::setSampleRate(double sr) {
        if (hostSampleRate != sr) { 
            hostSampleRate = sr; 
            recreateResamplers(); 
            updateFilters();
            reset(); 
        }
    }

    void ChannelProcessor::setMode(EraMode mode) {
        mode = distributionSafeMode(mode);
        if (currentMode != mode) {
            currentMode = mode;
            recreateResamplers();
            updateFilters();
            recreateCodec();
            ringCodecIn.reset(); ringCodecOut.reset();
        }
    }

    void ChannelProcessor::setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw) {
        bool changed = (evsSampleRate != sampleRateHz)
                    || (evsBitrateBps != bitrateBps)
                    || (evsMaxBandwidth != maxBw);
        evsSampleRate    = sampleRateHz;
        evsBitrateBps    = bitrateBps;
        evsMaxBandwidth  = maxBw;
#if TELEPHONY_USE_EVS_JBM
        // EVS_JBM uses the same EVS configuration knobs as EVS_NATIVE
        // (sample rate / bitrate / max bandwidth), so the same
        // change-detection path applies.
        const bool rebuildForEvs = (currentMode == EraMode::EVS_NATIVE
                                    || currentMode == EraMode::EVS_JBM);
#else
        const bool rebuildForEvs = (currentMode == EraMode::EVS_NATIVE);
#endif
        if (changed && rebuildForEvs) {
            recreateResamplers();
            recreateCodec();
            ringCodecIn.reset(); ringCodecOut.reset();
        }
    }

void ChannelProcessor::configure(bool artifacts, float amount, float packetLossRate, float networkDegradation) {
        const float clampedLoss = std::clamp(packetLossRate, 0.0f, 0.95f);
        const float clampedDegradation = std::clamp(networkDegradation, 0.0f, 1.0f);
        const bool networkChanged = std::fabs(paramNetworkDegradation - clampedDegradation) > 0.0001f;
        paramArtifactsEnabled = artifacts;
        paramArtifactAmount = amount;
        paramPacketLossRate = clampedLoss;
        paramNetworkDegradation = clampedDegradation;
        if (networkChanged) {
            updateFilters();
        }
        // Push the clamped values into the codec if one is currently
        // attached. Codecs that don't model a network (the default ICodec
        // base) treat this as a no-op; OpusCodec uses it to drive its
        // encoder CTLs and internal jitter-buffer state.
        if (codec) {
            codec->configureNetwork(clampedLoss, clampedDegradation);
        }
    }

void ChannelProcessor::reset() {
        ringCodecIn.reset(); ringCodecOut.reset(); outputBuffer.reset();
        hpFilter1.reset(); hpFilter2.reset();
        lpFilter1.reset(); lpFilter2.reset();
        packetLossBurstFrames = 0;
        simulatedPathPLC.reset(640);
        if (resamplerDown) resamplerDown->clear();
        if (resamplerUp) resamplerUp->clear();
        if (codec) codec->reset();
#if TELEPHONY_EXPERIMENTAL_NETWORK
        if (speexAux) speexAux->reset();
#endif
        updateFilters();
    }

    size_t ChannelProcessor::getAvailableOutput() const {
        return outputBuffer.getReadAvailable();
    }

    void ChannelProcessor::pushInput(const float* in, int numSamples) {
        if (currentMode == EraMode::Bypass) {
            outputBuffer.write(in, numSamples);
        } else {
            // Unified Resampling Path for all other modes (G.711, GSM, AMR, EVS)
            processCodec(numSamples, in);
        }
    }

    size_t ChannelProcessor::pullOutput(float* out, int numSamples) {
        size_t avail = outputBuffer.getReadAvailable();
        size_t count = (std::min)((size_t)numSamples, avail);
        outputBuffer.read(out, count);
        
        applyArtifacts(out, count);
        return count;
    }

    void ChannelProcessor::recreateResamplers() {
        resamplerDown.reset(); resamplerUp.reset();
        downMaxInLen = 0;
        upMaxInLen = 0;
        int targetSR = 0;
        switch (currentMode) {
            case EraMode::PSTN_G711:
            case EraMode::GSM_FR:
            case EraMode::AMR_NB_3G: targetSR = 8000; break;
            case EraMode::AMR_WB_VOLTE: targetSR = 16000; break;
            case EraMode::EVS_LIKE: targetSR = 32000; break;
            case EraMode::EVS_NATIVE: targetSR = evsSampleRate; break;
#if TELEPHONY_USE_EVS_JBM
            // EVS_JBM drives the EVS encoder+JBM at the configured
            // sample rate, so the resampler target mirrors EVS_NATIVE.
            case EraMode::EVS_JBM: targetSR = evsSampleRate; break;
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
            case EraMode::OPUS_VOIP: targetSR = 48000; break;
#endif
            default: targetSR = 0; break;
        }

        if (targetSR > 0) {
            // aMaxInLen is the maximum number of input samples that may be
            // passed to a single r8brain::CDSPResampler24::process() call.
            // We keep it at 1024 to match the historical buffer size, but
            // processCodec now chunks its calls so callers are free to push
            // more than this per pushInput().
            downMaxInLen = 1024;
            upMaxInLen = 1024;
            resamplerDown = std::make_unique<r8b::CDSPResampler24>(hostSampleRate, targetSR, downMaxInLen);
            resamplerUp = std::make_unique<r8b::CDSPResampler24>(targetSR, hostSampleRate, upMaxInLen);
        }
    }

    void ChannelProcessor::updateFilters() {
        float sampleRate = 0.0f;
        float highpassHz = 0.0f;
        float lowpassHz = 0.0f;
        float degradedFloorHz = 0.0f;

        auto applyBand = [&]() {
            if (sampleRate <= 0.0f || lowpassHz <= 0.0f) {
                hpFilter1.reset();
                lpFilter1.reset();
                return;
            }

            const float d = std::clamp(paramNetworkDegradation, 0.0f, 1.0f);
            highpassHz = highpassHz + d * ((std::max)(highpassHz, 260.0f) - highpassHz);
            lowpassHz = lowpassHz - d * (lowpassHz - degradedFloorHz);
            lowpassHz = std::clamp(lowpassHz, highpassHz + 300.0f, sampleRate * 0.45f);

            hpFilter1.setHighpass(highpassHz, sampleRate);
            lpFilter1.setLowpass(lowpassHz, sampleRate);
        };

        switch (currentMode) {
            case EraMode::PSTN_G711:
            case EraMode::GSM_FR:
                sampleRate = 8000.0f; // Filters apply at Codec SR
                highpassHz = 300.0f;
                lowpassHz = 3400.0f;
                degradedFloorHz = 1800.0f;
                applyBand();
                break;
            case EraMode::AMR_NB_3G:
                sampleRate = 8000.0f;
                highpassHz = 200.0f;
                lowpassHz = 3400.0f;
                degradedFloorHz = 1800.0f;
                applyBand();
                break;
            case EraMode::AMR_WB_VOLTE:
                sampleRate = 16000.0f;
                highpassHz = 50.0f;
                lowpassHz = 7000.0f;
                degradedFloorHz = 3400.0f;
                applyBand();
                break;
            case EraMode::EVS_LIKE:
                sampleRate = 32000.0f;
                highpassHz = 50.0f;
                lowpassHz = 14000.0f;
                degradedFloorHz = 4000.0f;
                applyBand();
                break;
            case EraMode::EVS_NATIVE:
                if (paramNetworkDegradation <= 0.001f) {
                    // The real EVS already shapes its own bandwidth; skip the
                    // extra cascade unless the radio/path simulation asks for it.
                    hpFilter1.reset();
                    lpFilter1.reset();
                } else {
                    sampleRate = (float)evsSampleRate;
                    highpassHz = 50.0f;
                    switch (evsMaxBandwidth) {
                        case EVS_NB:  lowpassHz = 3400.0f; break;
                        case EVS_WB:  lowpassHz = 7000.0f; break;
                        case EVS_SWB: lowpassHz = 14000.0f; break;
                        case EVS_FB:  lowpassHz = 20000.0f; break;
                        default:      lowpassHz = 14000.0f; break;
                    }
                    lowpassHz = (std::min)(lowpassHz, sampleRate * 0.45f);
                    degradedFloorHz = 3400.0f;
                    applyBand();
                }
                break;
#if TELEPHONY_USE_EVS_JBM
            case EraMode::EVS_JBM:
                // EVS_JBM runs the real EVS encoder + JBM at the
                // configured sample rate, so its filter profile
                // matches EVS_NATIVE.
                if (paramNetworkDegradation <= 0.001f) {
                    hpFilter1.reset();
                    lpFilter1.reset();
                } else {
                    sampleRate = (float)evsSampleRate;
                    highpassHz = 50.0f;
                    switch (evsMaxBandwidth) {
                        case EVS_NB:  lowpassHz = 3400.0f; break;
                        case EVS_WB:  lowpassHz = 7000.0f; break;
                        case EVS_SWB: lowpassHz = 14000.0f; break;
                        case EVS_FB:  lowpassHz = 20000.0f; break;
                        default:      lowpassHz = 14000.0f; break;
                    }
                    lowpassHz = (std::min)(lowpassHz, sampleRate * 0.45f);
                    degradedFloorHz = 3400.0f;
                    applyBand();
                }
                break;
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
            case EraMode::OPUS_VOIP:
                if (paramNetworkDegradation <= 0.001f) {
                    // Opus already shapes its own bandwidth (up to 20 kHz FB);
                    // skip the extra cascade unless the path simulation asks.
                    hpFilter1.reset();
                    lpFilter1.reset();
                } else {
                    sampleRate = 48000.0f;
                    highpassHz = 50.0f;
                    lowpassHz = 18000.0f;
                    degradedFloorHz = 3400.0f;
                    applyBand();
                }
                break;
#endif
            default:
                hpFilter1.reset();
                lpFilter1.reset();
                break;
        }
        // Cascade setup: Both stages identical for 24dB/oct
        hpFilter2 = hpFilter1;
        lpFilter2 = lpFilter1;
    }

    float ChannelProcessor::nextPacketRandom() {
        packetLossSeed = packetLossSeed * 1664525u + 1013904223u;
        return (float)((packetLossSeed >> 8) & 0x00FFFFFFu) / 16777216.0f;
    }

    bool ChannelProcessor::shouldDropPacket() {
        const float degradation = std::clamp(paramNetworkDegradation, 0.0f, 1.0f);
        const float derivedLoss = degradation * degradation * 0.08f;
        const float lossRate = std::clamp(paramPacketLossRate + derivedLoss, 0.0f, 0.95f);
        if (lossRate <= 0.0001f) return false;

        if (packetLossBurstFrames > 0) {
            --packetLossBurstFrames;
            return true;
        }

        const float meanBurstFrames = 1.0f + degradation * 3.0f;
        const float startProbability = lossRate / meanBurstFrames;
        if (nextPacketRandom() >= startProbability) return false;

        const int burst = 1 + (int)(nextPacketRandom() * meanBurstFrames);
        packetLossBurstFrames = (std::max)(0, burst - 1);
        return true;
    }

void ChannelProcessor::recreateCodec() {
        codec.reset();
        switch (currentMode) {
            case EraMode::PSTN_G711: codec = std::make_unique<G711Codec>(8000); break;
            case EraMode::GSM_FR: codec = std::make_unique<GSMCodec>(); break;
            case EraMode::AMR_NB_3G: codec = std::make_unique<AMRNBCodec>(); break;
            case EraMode::AMR_WB_VOLTE: codec = std::make_unique<AMRWBCodec>(); break;
            case EraMode::EVS_LIKE:
                codecFrameF.resize(32000 / 50);
                codecFrameSIn.resize(32000 / 50);
                codecFrameSOut.resize(32000 / 50);
                simulatedPathPLC.reset(32000 / 50);
                return;
            case EraMode::EVS_NATIVE:
                codec = std::make_unique<EVSCodec>(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
                break;
#if TELEPHONY_USE_EVS_JBM
            case EraMode::EVS_JBM:
                // EVSCodecJbm runs the real EVS encoder + Stage-1 JBM
                // in one ICodec. See the class header comment in
                // TelephonyDSP.h for the full design.
                codec = std::make_unique<EVSCodecJbm>(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
                break;
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
            case EraMode::OPUS_VOIP:
                // 48 kHz, 24 kbps, complexity 6 (moderate). See OpusCodec.
                codec = std::make_unique<OpusCodec>(48000, 24000, 6);
                break;
#endif
            default: break;
        }
        if (codec) {
            int fs = codec->getFrameSize();
            int sr = codec->getSampleRate();
            codecFrameF.resize(fs); codecFrameSIn.resize(fs); codecFrameSOut.resize(fs);
            simulatedPathPLC.reset(fs);
            // Push the current clamped network parameters into the freshly
            // created codec so its first packet already reflects the
            // caller's loss/degradation settings rather than defaults.
            codec->configureNetwork(paramPacketLossRate, paramNetworkDegradation);
#if TELEPHONY_EXPERIMENTAL_NETWORK
            // Bring the experimental SpeexDSP helper online with the same
            // rate/frame as the codec. It does not change audio output yet
            // (see SpeexDSPAux header comment for the follow-up plan).
            if (!speexAux) speexAux = std::make_unique<SpeexDSPAux>();
            speexAux->configure(sr, fs);
#endif
        }
    }

    void ChannelProcessor::prepareInternalBuffers(int maxBlockSize) {
        resampInBuf.resize(maxBlockSize);
        tempProcessBuf.resize(maxBlockSize);
    }

    // Process logic: Downsample -> Filter -> Codec -> Buffer (Wait for Upsample?)
    // Note: r8brain is streaming. Filters are streaming. Codec is block or sample based.
    // 1. Downsample (Host SR -> Codec SR)   -- chunked to never exceed aMaxInLen
    // 2. Filter (at Codec SR)
    // 3. Codec (at Codec SR)
    // 4. Upsample (Codec SR -> Host SR)     -- chunked to never exceed aMaxInLen
    //    -> outputBuffer
    void ChannelProcessor::processCodec(int numSamples, const float* in) {
         if (numSamples <= 0) return;

         // ---- Stage 1: downsample, chunked so each process() call receives
         //               at most downMaxInLen input samples ----
         if (downMaxInLen > 0) {
             if (resampInBuf.size() < (size_t)downMaxInLen) resampInBuf.resize(downMaxInLen);
             int inOffset = 0;
             while (inOffset < numSamples) {
                 const int inChunk = std::min(numSamples - inOffset, downMaxInLen);
                 for (int i = 0; i < inChunk; ++i)
                     resampInBuf[i] = (double)in[inOffset + i];

                 double* dOutRaw = nullptr;
                 const int downCount = resamplerDown->process(resampInBuf.data(), inChunk, dOutRaw);

                 // The pointer returned by r8brain is owned by the resampler
                 // and stays valid until the next process() call, so it is
                 // safe to consume within this iteration of the chunk loop.
                 if ((int)tempProcessBuf.size() < downCount) tempProcessBuf.resize(downCount);
                 for (int i = 0; i < downCount; ++i) {
                     float x = (float)dOutRaw[i];
                     x = hpFilter1.process(x);
                     x = hpFilter2.process(x);
                     x = lpFilter1.process(x);
                     x = lpFilter2.process(x);
                     tempProcessBuf[i] = x;
                 }
                 ringCodecIn.write(tempProcessBuf.data(), downCount);
                 inOffset += inChunk;
             }
         }

         // ---- Stage 2: codec/EVS_LIKE processing (unchanged) ----
         if (currentMode == EraMode::EVS_LIKE) {
             const int frameSize = 32000 / 50;
             while (ringCodecIn.getReadAvailable() >= (size_t)frameSize) {
                 ringCodecIn.read(codecFrameF.data(), frameSize);
                 for (int i = 0; i < frameSize; ++i) {
                     codecFrameSIn[i] = clampToInt16(codecFrameF[i] * 32767.0f);
                 }

                 if (shouldDropPacket()) {
                     simulatedPathPLC.conceal(codecFrameSOut.data(), frameSize);
                 } else {
                     std::memcpy(codecFrameSOut.data(), codecFrameSIn.data(), frameSize * sizeof(int16_t));
                     simulatedPathPLC.storeGoodFrame(codecFrameSOut.data(), frameSize);
                 }

                 for (int i = 0; i < frameSize; ++i) {
                     codecFrameF[i] = codecFrameSOut[i] / 32768.0f;
                 }
                 ringCodecOut.write(codecFrameF.data(), frameSize);
             }
         } else if (codec) {
             int frameSize = codec->getFrameSize();
             while (ringCodecIn.getReadAvailable() >= (size_t)frameSize) {
                 ringCodecIn.read(codecFrameF.data(), frameSize);
                 for(int i=0; i<frameSize; ++i) codecFrameSIn[i] = clampToInt16(codecFrameF[i] * 32767.0f);
                 codec->processFrame(codecFrameSIn.data(), codecFrameSOut.data(), shouldDropPacket());
                 for(int i=0; i<frameSize; ++i) codecFrameF[i] = codecFrameSOut[i] / 32768.0f;
                 ringCodecOut.write(codecFrameF.data(), frameSize);
             }
         }

         // ---- Stage 3: upsample, chunked so each process() call receives
         //               at most upMaxInLen input samples ----
         int codecOutAvail = (int)ringCodecOut.getReadAvailable();
         if (codecOutAvail > 0 && upMaxInLen > 0) {
             if ((int)tempProcessBuf.size() < codecOutAvail) tempProcessBuf.resize(codecOutAvail);
             ringCodecOut.read(tempProcessBuf.data(), codecOutAvail);

             if (resampInBuf.size() < (size_t)upMaxInLen) resampInBuf.resize(upMaxInLen);
             int upOffset = 0;
             while (upOffset < codecOutAvail) {
                 const int upChunk = std::min(codecOutAvail - upOffset, upMaxInLen);
                 for (int i = 0; i < upChunk; ++i)
                     resampInBuf[i] = (double)tempProcessBuf[upOffset + i];

                 double* dUpOutRaw = nullptr;
                 const int upCount = resamplerUp->process(resampInBuf.data(), upChunk, dUpOutRaw);

                 // r8brain's output pointer is invalidated by the next
                 // process() call, so copy it into a member-scope scratch
                 // buffer (resampUpOutBuf) and write to outputBuffer before
                 // looping. Streaming order is preserved by chunking in input
                 // order.
                 if ((int)resampUpOutBuf.size() < upCount) resampUpOutBuf.resize(upCount);
                 for (int i = 0; i < upCount; ++i)
                     resampUpOutBuf[i] = (float)dUpOutRaw[i];
                 outputBuffer.write(resampUpOutBuf.data(), upCount);
                 upOffset += upChunk;
             }
         }
    }
    
    void ChannelProcessor::applyArtifacts(float* buffer, int numSamples) {
        if (!paramArtifactsEnabled || paramArtifactAmount <= 0.001f) return;
        static uint32_t seed = 12345;
        auto randf = [&]() { seed = seed * 1664525 + 1013904223; return (float)(seed & 0xFFFF) / 32768.0f - 1.0f; };
        for (int i=0; i<numSamples; ++i) {
            if (currentMode == EraMode::GSM_FR && (rand() % 1000) < (10 * paramArtifactAmount)) buffer[i] += randf() * 0.5f * paramArtifactAmount;
            // General noise
            buffer[i] += randf() * 0.01f * paramArtifactAmount;
        }
    }

    SignalProcessor::SignalProcessor()
        : hostSampleRate(44100.0), currentMode(EraMode::Bypass),
          routeModelEnabled(false),
          inputEndpoint(RouteEndpoint::FixedLine), outputEndpoint(RouteEndpoint::Mobile5G),
          degradationSegment(DegradationSegment::Both),
          paramDryWet(1.0f), paramOutGain(1.0f), paramArtifactsEnabled(false), paramArtifactAmount(0.0f),
          paramPacketLossRate(0.0f), paramNetworkDegradation(0.0f),
          simulateLatency(true),
          evsSampleRate(32000), evsBitrateBps(EVS_BR_13200), evsMaxBandwidth(EVS_SWB),
          targetLatencySamples(0), inTotalSamples(0), outTotalSamples(0)
    {
        updateLatency();
    }

    SignalProcessor::~SignalProcessor() {}

    EraMode SignalProcessor::endpointToMode(RouteEndpoint endpoint) const {
        switch (endpoint) {
            case RouteEndpoint::FixedLine:       return EraMode::PSTN_G711;
            case RouteEndpoint::Mobile2G:        return EraMode::GSM_FR;
            case RouteEndpoint::Mobile3G:        return distributionSafeMode(EraMode::AMR_NB_3G);
            case RouteEndpoint::Mobile4G:        return distributionSafeMode(EraMode::AMR_WB_VOLTE);
            case RouteEndpoint::Mobile5G:        return EraMode::EVS_LIKE;
            case RouteEndpoint::Mobile5GNative:  return distributionSafeMode(EraMode::EVS_NATIVE);
            default:                             return EraMode::Bypass;
        }
    }

    bool SignalProcessor::degradesInputLeg() const {
        return degradationSegment == DegradationSegment::Both
            || degradationSegment == DegradationSegment::InputToExchange;
    }

    bool SignalProcessor::degradesOutputLeg() const {
        return degradationSegment == DegradationSegment::Both
            || degradationSegment == DegradationSegment::ExchangeToOutput;
    }

    void SignalProcessor::applyRouteToChannels() {
        for (size_t i = 0; i < outputLegs.size(); ++i) {
            auto& inputLeg = inputLegs[i];
            auto& outputLeg = outputLegs[i];
            inputLeg->setEVSConfig(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
            outputLeg->setEVSConfig(evsSampleRate, evsBitrateBps, evsMaxBandwidth);

            if (routeModelEnabled) {
                inputLeg->setMode(endpointToMode(inputEndpoint));
                outputLeg->setMode(endpointToMode(outputEndpoint));

                const float inputLoss = degradesInputLeg() ? paramPacketLossRate : 0.0f;
                const float inputDeg = degradesInputLeg() ? paramNetworkDegradation : 0.0f;
                const float outputLoss = degradesOutputLeg() ? paramPacketLossRate : 0.0f;
                const float outputDeg = degradesOutputLeg() ? paramNetworkDegradation : 0.0f;

                inputLeg->configure(paramArtifactsEnabled, paramArtifactAmount, inputLoss, inputDeg);
                outputLeg->configure(paramArtifactsEnabled, paramArtifactAmount, outputLoss, outputDeg);
            } else {
                inputLeg->setMode(EraMode::Bypass);
                inputLeg->configure(false, 0.0f, 0.0f, 0.0f);
                outputLeg->setMode(currentMode);
                outputLeg->configure(paramArtifactsEnabled, paramArtifactAmount,
                                     paramPacketLossRate, paramNetworkDegradation);
            }
        }
    }

    void SignalProcessor::setSampleRate(double sr) {
        hostSampleRate = sr;
        updateLatency();
        for (auto& ch : inputLegs) ch->setSampleRate(sr);
        for (auto& ch : outputLegs) ch->setSampleRate(sr);
        reset();
    }

    void SignalProcessor::setMode(EraMode mode) {
        mode = distributionSafeMode(mode);
        routeModelEnabled = false;
        currentMode = mode;
        applyRouteToChannels();
    }

    void SignalProcessor::setRoute(RouteEndpoint input, RouteEndpoint output, DegradationSegment segment) {
        routeModelEnabled = true;
        inputEndpoint = input;
        outputEndpoint = output;
        degradationSegment = segment;
        applyRouteToChannels();
    }

    void SignalProcessor::setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw) {
        evsSampleRate   = sampleRateHz;
        evsBitrateBps   = bitrateBps;
        evsMaxBandwidth = maxBw;
        for (auto& ch : inputLegs) ch->setEVSConfig(sampleRateHz, bitrateBps, maxBw);
        for (auto& ch : outputLegs) ch->setEVSConfig(sampleRateHz, bitrateBps, maxBw);
    }

    void SignalProcessor::setParameters(float dryWet, float outGaindB, bool artifacts, float artifactAmount,
                                        float packetLossRate, float networkDegradation) {
        paramDryWet = dryWet;
        paramOutGain = std::pow(10.0f, outGaindB / 20.0f);
        paramArtifactsEnabled = artifacts;
        paramArtifactAmount = artifactAmount;
        paramPacketLossRate = std::clamp(packetLossRate, 0.0f, 0.95f);
        paramNetworkDegradation = std::clamp(networkDegradation, 0.0f, 1.0f);
        applyRouteToChannels();
    }

    void SignalProcessor::setSimulateLatency(bool enable) {
        simulateLatency = enable;
        updateLatency();
    }

    void SignalProcessor::reset() {
        inTotalSamples = 0;
        outTotalSamples = 0;
        for (auto& ch : inputLegs) ch->reset();
        for (auto& ch : outputLegs) ch->reset();
        for (auto& db : dryBuffers) db->reset();
        applyRouteToChannels();
    }

    void SignalProcessor::updateLatency() {
        if (simulateLatency) targetLatencySamples = (int)std::round(0.1 * hostSampleRate);
        else targetLatencySamples = 0;
    }

    int SignalProcessor::getLatencySamples() const {
        return targetLatencySamples;
    }

    void SignalProcessor::ensureChannels(int count) {
        if (outputLegs.size() != (size_t)count) {
            inputLegs.clear();
            outputLegs.clear();
            dryBuffers.clear();
            for (int i=0; i<count; ++i) {
                inputLegs.push_back(std::make_unique<ChannelProcessor>(hostSampleRate));
                outputLegs.push_back(std::make_unique<ChannelProcessor>(hostSampleRate));
                dryBuffers.push_back(std::make_unique<RingBuffer>(131072));
            }
            applyRouteToChannels();
        }
    }

    void SignalProcessor::process(float** inputs, int numIns, float** outputs, int numOuts, int numSamples) {
        ensureChannels(numIns);
        inTotalSamples += numSamples;

        for (int i=0; i<numIns; ++i) {
            dryBuffers[i]->write(inputs[i], numSamples);
            if (routeModelEnabled) {
                std::vector<float> exchange(numSamples, 0.0f);
                inputLegs[i]->pushInput(inputs[i], numSamples);
                inputLegs[i]->pullOutput(exchange.data(), numSamples);
                outputLegs[i]->pushInput(exchange.data(), numSamples);
            } else {
                outputLegs[i]->pushInput(inputs[i], numSamples);
            }
        }

        int64_t readable = (inTotalSamples - targetLatencySamples) - outTotalSamples;
        int samplesToWrite = numSamples;
        int outputOffset = 0;

        if (readable < 0) {
            int silence = (int)(std::min)((int64_t)samplesToWrite, -readable);
            for (int ch=0; ch<numOuts; ++ch) {
                std::memset(outputs[ch], 0, silence * sizeof(float));
            }
            samplesToWrite -= silence;
            outputOffset += silence;
            outTotalSamples += silence;
        }

        if (samplesToWrite > 0) {
            std::vector<std::vector<float>> processedChannels(numIns, std::vector<float>(samplesToWrite));
            
            for (int i=0; i<numIns; ++i) {
                std::vector<float> d(samplesToWrite);
                if (dryBuffers[i]->getReadAvailable() >= (size_t)samplesToWrite) {
                    dryBuffers[i]->read(d.data(), samplesToWrite);
                } else {
                    dryBuffers[i]->read(d.data(), dryBuffers[i]->getReadAvailable());
                }

                std::vector<float> w(samplesToWrite);
                outputLegs[i]->pullOutput(w.data(), samplesToWrite);
                
                for (int s=0; s<samplesToWrite; ++s) {
                    processedChannels[i][s] = (d[s] * (1.0f - paramDryWet) + w[s] * paramDryWet) * paramOutGain;
                }
            }

            for (int ch=0; ch<numOuts; ++ch) {
                int inCh = (ch < numIns) ? ch : 0;
                float* dest = outputs[ch] + outputOffset;
                std::memcpy(dest, processedChannels[inCh].data(), samplesToWrite * sizeof(float));
            }
            
            outTotalSamples += samplesToWrite;
        }
    }
}
