# Feature acceptance matrix

This checklist preserves every Tier 1–4 request. “Implemented” means an actual
local audio/packet path plus regression tests, not a renamed control or a
synthetic readout. It does not mean certified radio/codec conformance.
All network operations below are simulated in memory. No sockets or live calls.

| ID | Implementation / user entry point | Acceptance evidence and limits |
|---|---|---|
| 1.1 | Independent network page and `NetworkProfile`: band, filter cascade, jitter, burst/loss, FEC and playout controls | Codec queues and filters receive separate settings; `advanced_integration`, `transport_integration` |
| 1.2 | Good/Bad Gilbert–Elliott p/r/k/h, independent loss and mean-burst conversion | Seeded statistical tests. **EP1–EP6 reference traces remain external-data work**, see below |
| 1.3 | Uniform, Gamma, Weibull, Pareto; AR(1) correlation | Reproducibility, moments/bounds, correlation and invalid-input tests |
| 1.4 | `--ber` / Payload BER; encoded AMR/EVS/Opus/G.711/GSM payload corruption with container/control fields protected | Real codecs encode/corrupt/decode under ASan; not corrupt PCM after decoding |
| 1.5 | Pure-silence DTX comparison | Native SID/NO_DATA or explicit legacy VAD decisions; delayed packets retain their DTX state |
| 1.6 | Early/late/duplicate arrivals, arrival-ordered delivery and sequence-ordered playout | Encoded byte queues, retained FEC look-ahead, loss/late/duplicate counters |
| 2.1 | PSD-shaped noise learned from good frames; optional legacy repetition for comparison | Spectral shape/RMS, finite output, deterministic reset; no speech waveform replay in PSD mode |
| 2.2 | Q.23 dual-frequency DTMF, 16 keys, tone/gap/level controls before codec | Frequency/level/timing tests, silent-input whole-DSP output test; no Q.24 receiver certification |
| 2.3 | Host timeline mode, project sample clock (musical time/tempo fallback), seek reset | Processor host-context and seek tests; deterministic sample clock when disabled; CLI rejects host-only mode |
| 2.4 | Opus 2.5/5/10/20/40/60 ms, actual packet mode, pinned private force-mode CTL, FEC controls | Real duration/ToC/LBRR tests. Force CTL is private to pinned Opus, not promised as portable public API |
| 2.5 | EVS spectral auto-bandwidth with hysteresis, native and JBM | Narrowband→fullband synthetic inputs and live configuration tests |
| 2.6 | State-preserving AMR/EVS rate changes, 5 ms filter-coefficient interpolation and optional bounded mode-change artifact | Continuity/reset tests. Sample-rate changes re-prime the stream; native/IO profile restoration tested |
| 2.7 | Legacy speech gating/DTX; native AMR/EVS SID/CNG remains native | Energy+hangover or available VAD2; pure-silence alignment through delayed queues |
| 2.8 | Real EVS AMR-WB IO at all nine rates, native↔IO transitions and SID metadata | Reference codec and payload tests. JBM IO is unavailable and rejected/disabled; use native EVS |
| 2.9 | Basic plus four advanced GUI pages, Opus routes, live dBFS peaks and measured loss/jitter/mode | Host automation, state, keyboard/layout, latency notification and SDK validator tests. Actual DAW visual/audition QA remains necessary |
| 3.1 | Serialized RTP V2 headers passed through actual encoded queues | Known vectors, padding/extensions/CSRC checks, malformed input and wrap tests |
| 3.2 | RFC 4867 mono AMR-NB/WB octet-aligned and bandwidth-efficient payloads | All 17 codec modes, real SID/NO_DATA; identical decoded PCM. Optional CRC/interleaving/robust sorting not negotiated |
| 3.3 | EVS Compact and Header-Full, CMR/ToC, native G.192 and IO sensitivity ordering | All defined modes, ambiguity/padding validation, real encoded-frame round trips |
| 3.4 | ±50 ppm receiver-clock drift with bounded elastic buffering; JBM uses its reference receiver/time scaler | Drift ratio, slip/continuity, asynchronous sender/receiver and actual JBM APA call/output tests. General-codec elastic resampling is not relabeled EVS APA |
| 3.5 | Stable filtered actual decoded-output feedback to the encoder input | Echo delay = configured line delay + one codec frame; stability/decay and reset tests |
| 3.6 | RFC 2198 previous-packet redundancy and Opus FEC-only decode | Actual RED recovery and `opus_decode(..., decode_fec=1)`; **Opus ToC has no FEC flag** |
| 3.7 | Actual RR/SR/XR reference-time packets, sequence/loss/jitter accounting and GUI readouts | Report parsing/counters/LSR/DLSR tests. No network RTT or complete conferencing RTCP session |
| 4.1 | Seeded Rayleigh sum-of-sinusoids fading, smoothed C/I and adaptive AMR/EVS rates | Distribution/correlation/stability tests. **Engineering model, not validated TS 45.008/36.101 RF conformance** |
| 4.2 | Periodic 50–200 ms handover interruption with recovery envelope while codec state continues | Exact interruption/recovery timing and whole-stream block independence |
| 4.3 | Complete official TS 26.442 v16.4 fixed-point source via user-supplied path; no former link-only stubs | 80 synthetic configurations + runtime profiles byte-equal to official CLI, ASan. Source not redistributed; official TS 26.444 suite not run |
| 4.4 | Wideband path followed by real 8 kHz G.711 tandem encode/decode and reconstruction | High-band attenuation/audio change, variable Opus framing, reset/partition invariance |
| 4.5 | Actual bundled OpenCORE VAD2 spectral core, primary decision/burst smoothing/hangover | Reference-core and streaming tests; standalone resampling/LTP front-end is not full-encoder bit-exact. Unavailable in distribution build |

## Remaining external evidence / acceptance work

1. **EP1–EP6:** the original roadmap attribution was wrong. These are TR 45.050
   Annex F.2 GSM half-rate chip/channel error files, not TS 26.131 packet-loss
   presets. The exact files, source and encoding are in
   [TRANSPORT_MODELS.md](TRANSPORT_MODELS.md). They are not present here. Raw radio
   error traces must not be guessed or treated as one lost RTP packet per bit.
   The lower-level custom 0/1 packet-pattern API is implemented; GUI/CLI import
   of normative radio traces is not implemented without that mapping/data.
2. **Radio conformance:** an RF modulation/channel/receiver and a specified
   profile/reference data set are needed to claim TS 45.008/36.101 behavior.
   Current deterministic audio degradation/rate adaptation is useful and tested,
   but is explicitly an engineering model, not an RF certification simulator.
3. **Official FX conformance:** the full official source is required locally;
   licensing/patent rights are not inferred from availability. The large official
   TS 26.444 BASOP vector suite remains unexecuted. Synthetic reference-CLI byte
   equality is reported separately. See [EVS_FIXED_POINT.md](EVS_FIXED_POINT.md).
4. **Native GUI/audition:** XML/parameter/SDK tests do not substitute for opening
   the plugin in a real DAW and listening through an actual host audio path.

These items remain visible; they are not removed from the requested scope or
marked complete by documentation edits.
