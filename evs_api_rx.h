#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 3GPP EVS Stage-1 JBM / VoIP receive adapter (experimental)
//
// Thin parent-repo wrapper around the 3GPP EVS reference's
// `external/3gpp-evs/lib_dec/EvsRXlib.h` ("Stage-1" RTP jitter-buffer +
// time-scaler + decoder wrapper used by the reference CLI's `-VOIP` path).
//
// This header lives at the top level of the TelephonyVoice repo on purpose:
// the JBM is intentionally not wired into TelephonyDSP or the VST3 / CLI
// pipeline yet, so the float-EVS internals stay sealed inside the submodule
// and the only symbols added to TelephonyDSP are the adapter entry points
// guarded by the CMake option TELEPHONY_USE_EVS_JBM.
//
// IMPORTANT: This adapter is only built for the floating-point EVS variant
// (TELEPHONY_USE_EVS_FX=OFF), and it is *not* part of the distribution build.
// The fixed-point variant does not currently link EvsRXlib.c (the fixed-point
// equivalent would need separate wiring) and the distribution build excludes
// the entire 3GPP EVS reference library.
//
// Bitstream contract (raw AU):
//
//   - `au_bits` points to the compact EVS access-unit payload exactly as the
//     reference encoder emits it: the raw bytes produced by
//     `evs_enc_process` / the AU bytes placed on the wire by a 3GPP EVS RTP
//     sender. NOT the G.192 short-stream format used by the reference CLI's
//     file I/O. EvsRXlib's `EVS_RX_FeedFrame` treats this buffer as a packed
//     MSB-first bit array (see `JB4_DATAUNIT::data`).
//
//   - `au_bits_count` is the payload size in BITS, not bytes. `EVS_RX_FeedFrame`
//     stores `dataSize = au_bits_count` and rounds the byte-copy up to
//     `(au_bits_count + 7) / 8`, so a non-multiple-of-eight bit count is
//     allowed (the trailing bits in the last byte are ignored by the
//     reference decoder). Values 1 .. MAX_BITS_PER_FRAME (2560) are accepted.
//
//   - `rtp_timestamp_ms` is the JBM's 1 ms-scale timestamp. EvsRXlib sets
//     `dataUnit->timeStamp = rtpTimeStamp` and uses it directly to place
//     the frame on the playout timeline. The adapter expects the value
//     already converted to milliseconds / JBM's 1000 Hz time; for RTP
//     timestamps, pass a timestamp delta converted as
//     `rtp_delta / (rtp_clock_hz / 1000)` relative to the first packet.
//
//   - `receive_time_ms` / `system_time_ms` are wall-clock times in
//     milliseconds. The JBM aligns playout to `system_time_ms` and uses
//     `receive_time_ms` only to decide which packets are "in the past"
//     relative to the system clock. Both are ms, EVS operates on a fixed
//     20 ms cadence (50 frames per second).
//
// Output PCM:
//   16-bit linear PCM, `sample_rate_hz/50` samples per call (e.g. 320 at
//   16 kHz, 960 at 48 kHz). The JBM/time-scaler stages happen entirely
//   inside the adapter; the caller only sees one decoded frame per call.
//
// Lifecycle:
//   `evs_rx_jbm_destroy` calls `EVS_RX_Close`, which internally calls
//   `destroy_decoder()` on the embedded `Decoder_State` and frees the
//   jitter buffer, time-scaler and FIFO. The adapter then frees the
//   wrapper and its scratch buffers. Do not call `evs_rx_jbm_destroy`
//   twice on the same handle.
// ---------------------------------------------------------------------------

#include <stddef.h>

// Reuse the project's EVS_OK / EVS_ERROR enum from evs_api.h. The adapter
// is only compiled alongside evs_api.c (see CMakeLists.txt), so the include
// resolves to the float-variant header that defines EVS_Status.
#include "evs_api.h"

typedef struct EVS_RxJbm EVS_RxJbm;
// Actual reference decoder SID/no-data state, after the latest PCM pull.
int evs_rx_jbm_in_dtx(const EVS_RxJbm* rx);
int evs_rx_jbm_has_started(const EVS_RxJbm* rx);
// Explicit margin, including zero; legacy create(…,0) keeps its 60ms default.
EVS_RxJbm* evs_rx_jbm_create_ex(int sample_rate_hz, int bitrate_bps, int safety_margin_ms);

// ---------------------------------------------------------------------------
// Create / destroy.
//
// sample_rate_hz : 8000, 16000, 32000 or 48000. The decoder and time-scaler
//                  are configured for this output rate and each
//                  evs_rx_jbm_get_samples() call returns exactly
//                  sample_rate_hz/50 samples.
//
// bitrate_bps    : EVS bitrate the sender is using, in bps (5900..128000).
//                  The first received frame's RTP payload decides the
//                  actual codec mode internally; this argument is the hint
//                  that the existing non-JBM decoder uses.
//
// jbm_safety_margin_ms
//                : JBM safety margin (delay reserve) in milliseconds. The
//                  reference CLI default is 60. Values <= 0 are clamped to
//                  60 inside the adapter. Larger values give more room
//                  against late packets at the cost of algorithmic delay.
//
// Returns NULL on validation failure or allocation failure.
// ---------------------------------------------------------------------------
EVS_RxJbm* evs_rx_jbm_create(int sample_rate_hz,
                             int bitrate_bps,
                             int jbm_safety_margin_ms);

void       evs_rx_jbm_destroy(EVS_RxJbm* rx);

// ---------------------------------------------------------------------------
// Feed one received RTP access unit into the JBM.
//
// au_bits               : raw EVS AU payload (MSB-first packed bytes; see
//                         file-level comment above).
// au_bits_count         : payload size in BITS (1 .. MAX_BITS_PER_FRAME).
// rtp_sequence_number   : 16-bit RTP sequence number.
// rtp_timestamp_ms      : RTP timestamp on the JBM's 1 ms scale.
// receive_time_ms       : wall-clock receive time in ms.
//
// Returns EVS_OK on success, EVS_ERROR on invalid arguments or JBM failure.
// ---------------------------------------------------------------------------
int evs_rx_jbm_feed_frame(EVS_RxJbm* rx,
                          const unsigned char* au_bits,
                          int au_bits_count,
                          unsigned short rtp_sequence_number,
                          unsigned long  rtp_timestamp_ms,
                          unsigned int  receive_time_ms);

// ---------------------------------------------------------------------------
// Pull one 20 ms frame of decoded 16-bit PCM from the JBM.
//
// pcm_out               : caller-allocated buffer; must hold at least
//                         sample_rate_hz/50 short samples.
// pcm_capacity_samples  : capacity of pcm_out in samples. Must be >=
//                         sample_rate_hz/50; the adapter does not enforce
//                         a larger minimum, but the adapter's internal
//                         scratch must accommodate the time-scaler
//                         (always APA_BUF = 12288 internally; only
//                         sample_rate_hz/50 is copied to the caller).
// system_time_ms        : current wall-clock time in ms. The JBM uses this
//                         to decide whether to pop a real frame, conceal
//                         a missing one, or stretch the previous frame.
// n_samples             : on return, number of samples written to pcm_out
//                         (always sample_rate_hz/50 on success).
//
// Returns EVS_OK on success, EVS_ERROR otherwise.
// ---------------------------------------------------------------------------
int evs_rx_jbm_get_samples(EVS_RxJbm* rx,
                           short* pcm_out,
                           int pcm_capacity_samples,
                           unsigned int system_time_ms,
                           int* n_samples);

// ---------------------------------------------------------------------------
// Channel-aware (RF) FEC offset currently held by the JBM.
//
// offset                : optimum FEC offset as a short (frames).
// fec_hi                : non-zero => LO/HI selector is HI (see
//                         JB4_FECoffset).
//
// Returns EVS_OK on success.
// ---------------------------------------------------------------------------
int evs_rx_jbm_get_fec_offset(EVS_RxJbm* rx, int* offset, int* fec_hi);

// ---------------------------------------------------------------------------
// Returns 1 if the jitter buffer is currently empty, 0 if not empty,
// or EVS_ERROR (-1) if the handle is invalid.
//
// Intended for end-of-stream draining only; the reference library explicitly
// notes "not during normal operation".
// ---------------------------------------------------------------------------
int evs_rx_jbm_is_empty(EVS_RxJbm* rx);

// ---------------------------------------------------------------------------
// Convert a G.192 short-stream (the format produced by the reference
// encoder's evs_enc_process bitstream output) into the compact MSB-first
// EVS access-unit bytes that evs_rx_jbm_feed_frame expects.
//
// bitstream         : the raw bytes returned by evs_enc_process, treated as
//                     a native-endian array of unsigned short (uint16).
// bitstream_used    : number of valid bytes in `bitstream`.
// compact           : caller-allocated output buffer for the compact AU.
// compact_capacity  : capacity of `compact` in bytes.
//
// On success returns the number of valid bits in the AU (1..MAX_BITS_PER_FRAME,
// matching the second uint16 word in the G.192 short-stream). Returns
// EVS_ERROR if the input is malformed, nb_bits is out of range, or the
// compact buffer is too small (must be at least 320 bytes to cover the
// 2560-bit worst case).
//
// Semantics: the G.192 short-stream layout is
//   word[0] = SYNC_WORD (0x6B21, validated)
//   word[1] = nb_bits  (number of payload bits that follow)
//   word[2..2+nb_bits-1] = payload bits as 0x007F (zero) / 0x0081 (one)
// This helper walks the payload words, sets bit i in the output MSB-first
// whenever word[2+i] == 0x0081, and returns nb_bits. The output's byte
// count is therefore ceil(nb_bits / 8); the caller passes that count (in
// bits) to evs_rx_jbm_feed_frame.
// ---------------------------------------------------------------------------
int evs_rx_jbm_g192_to_compact_au(const unsigned char* bitstream,
                                  int bitstream_used,
                                  unsigned char* compact,
                                  int compact_capacity);

#ifdef __cplusplus
}
#endif
