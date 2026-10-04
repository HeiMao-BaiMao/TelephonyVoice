# Implementation Roadmap / 実装ロードマップ

All items are VST-plugin-internal only. No external network I/O, no
multi-party conferencing, no signaling protocol stacks. The goal is
perfect emulation of telephony noise, voice quality, packet loss, and
radio degradation within the DAW plugin.

Difficulty estimates: ★ = trivial (<50 lines), ★★ = small (50~150 lines),
★★★ = medium (1~3 files, 150~500 lines), ★★★★ = large (cross-cutting,
500+ lines), ★★★★★ = architecture redesign.

---

## Tier 0 — codec control points ✅ COMPLETED (`b354da6`)

| # | Item | Difficulty |
|---|------|-----------|
| 0.0 | File-structure refactor: `TelephonyDSP.h/cpp` → `dsp/` (27 files) | ★★★★★ |
| 0.1 | **G.711 A-law / μ-law selection** — `G711Codec::setLaw()`, VST + CLI | ★ |
| 0.2 | **EVS DTX SID interval** — `EVSCodec::setDtxSidInterval()` (0=variable, 3~100) | ★ |
| 0.3 | **Opus variable bitrate** — `OpusCodec::setBitrate()` (6~256 kbps) | ★ |
| 0.4 | **AMR-WB 9 modes** — `AMRWBCodec::setMode()` (6.60~23.85 kbps) | ★★ |
| 0.5 | **AMR-NB 8 modes** — `AMRNBCodec::setMode()` (4.75~12.2 kbps) | ★★ |
| 0.6 | **EVS SC-VBR** — `EVSCodec::setScVbrEnabled()` (5.9 kbps mode) | ★★ |
| 0.7 | **Opus bandwidth** — `OpusCodec::setMaxBandwidth()` (NB~FB) | ★★ |

---

## Tier 1 — network simulation fidelity

The current `networkDegradation` slider couples 7 effects into one axis.
Each item below decouples them or adds new independent simulation knobs.

| # | Item | Difficulty |
|---|------|-----------|
| 1.1 | **Degradation parameter separation** — split `networkDegradation` into independent controls for: bandwidth narrowing, jitter amplitude, burst length mean, loss-rate boost, Opus FEC percent, Opus playback delay, filter-cascade enable. New `NetworkProfile` struct + VST knobs (6~8 new params). | ★★★★ |
| 1.2 | **Gilbert-Elliott 2-state Markov loss model** — replace the current simplified burst model with proper Good↔Bad state transitions (p, r, k, h parameters). Add 3GPP TS 26.131 error patterns (EP1~EP6) as presets. | ★★★ |
| 1.3 | **Jitter distribution upgrade** — change from uniform LCG jitter to Gamma / Weibull / Pareto distributions with configurable shape parameters. Add AR(1) autocorrelation for bursty jitter (consecutive frames with correlated delays). | ★★★ |
| 1.4 | **Bit-error injection (BER)** — flip random bits in the encoded bitstream before decoding. Per-codec support: AMR bitstream corruption, EVS G.192 bit-flip, Opus ToC-aware corruption. New `--ber` CLI flag + VST slider. | ★ |
| 1.5 | **Pure-silence DTX comparison mode** — emit true zero-PCM on DTX silence (bypass CNG) for A/B comparison. | ★ |
| 1.6 | **Packet reordering** — allow jitter offsets to go negative (early arrivals), generating out-of-order seq delivery. Queue upgrade to `seq`-sorted multiset. Late-packet and duplicate-packet simulation. | ★★★★ |

---

## Tier 2 — audio realism & comfort-noise quality

| # | Item | Difficulty |
|---|------|-----------|
| 2.1 | **PSD-based comfort noise for G.711 / GSM / EVS_LIKE** — replace the current waveform-repetition PLC in non-DTX modes with proper spectrally-shaped comfort noise. Estimate PSD from recent good frames, generate colored noise, apply gain matching. | ★★★ |
| 2.2 | **DTMF tone generation** — dual sine-wave generation with standard ITU-T frequencies and timing (50~100 ms on, 50 ms gap). Insert as test signal before the codec stage. | ★★ |
| 2.3 | **Real-time transport mode** — use VST3 `ProcessContext::projectTimeMusic` for packet arrival times instead of the deterministic `frameIndex * 20 ms` clock. Togglable vs. deterministic mode. | ★★ |
| 2.4 | **Opus detailed FEC controls** — expose `OPUS_SET_EXPERT_FRAME_DURATION` (2.5/5/10/20/40/60 ms), `OPUS_GET_MODE` (SILK/hybrid/CELT readout), `OPUS_SET_FORCE_MODE`. | ★★★ |
| 2.5 | **EVS auto-bandwidth switching** — lightweight spectrum estimator on input → dynamically lower `max_bwidth` (NB→WB→SWB→FB) based on signal content. | ★★★ |
| 2.6 | **Codec mode-switching transient artifacts** — simulate audible clicks / bandwidth-transition artifacts when AMR/EVS changes bitrate mid-call. Requires filter-coefficient interpolation and encoder-state preservation across mode changes. | ★★★★ |
| 2.7 | **VAD→DTX extension to AMR / G.711 / GSM** — wire the existing SpeexDSP energy VAD to the legacy codecs (currently only OPUS_VOIP / EVS_LIKE). | ★ |
| 2.8 | **EVS AMR-WB IO mode** — enable the 3GPP EVS encoder's inter-op mode that produces AMR-WB-compatible bitstreams. Requires `evs_dec_create` signature change (breaking). | ★★★ |
| 2.9 | **VSTGUI editor** — route diagram, codec labels, configured-loss readout, grouped controls, host automation binding and zoom are implemented. Live VU meters and measured loss telemetry remain future work; the editor does not present simulated readouts as measurements. | ★★★★★ |

---

## Tier 3 — protocol & transport fidelity (internal simulation)

No actual network sockets. All protocol layers are simulated in-process for
realistic codec/PCL/JBM behavior.

| # | Item | Difficulty |
|---|------|-----------|
| 3.1 | **RTP header internal simulation** — build/serialize/parse 12-byte RTP fixed headers (V=2, P, X, CC, M, PT, seq, ts, SSRC). Feed serialized RTP packets through the internal jitter queue so codecs see RFC-compliant payloads. | ★★★ |
| 3.2 | **RFC 4867 AMR payload format** — octet-aligned and bandwidth-efficient modes with CMR, ToC, and speech bits packing/unpacking. | ★★★★ |
| 3.3 | **TS 26.445 EVS payload format** — header-full (CMR + ToC + payload) and compact modes. G.192 ↔ RFC payload conversion. | ★★★★ |
| 3.4 | **Clock drift simulation** — asymmetric sample-rate offsets (±5~50 ppm) between encoder and decoder. Periodic buffer underrun/overrun → APA time-scaler activation. | ★★★★ |
| 3.5 | **Hybrid echo (line echo)** — PSTN 2-wire/4-wire hybrid simulation: delayed, attenuated, filtered feedback of output into input path. Requires `SignalProcessor` architecture change (feedback loop). | ★★★★★ |
| 3.6 | **RED (RFC 2198) + Opus FEC-only mode** — redundant audio payload encoding and Opus ToC-byte FEC indicator bit handling. | ★★★★ |
| 3.7 | **RTCP receiver reports** — collect jitter, loss fraction, cumulative lost packets over the internal simulation timeline. Generate RTCP SR/RR/XR packets. Display as VST readouts (no network transmission). | ★★★ |

---

## Tier 4 — wireless channel & deep simulation

| # | Item | Difficulty |
|---|------|-----------|
| 4.1 | **Wireless fading / C-I rate adaptation** — Rayleigh/Jakes fading model → C/I estimation → dynamic AMR/EVS mode selection per 3GPP TS 45.008 / 36.101 channel models. | ★★★★★ |
| 4.2 | **Handover gap simulation** — momentary mute (50~200 ms) with rapid codec-state recovery, mimicking inter-base-station handovers. | ★★★★ |
| 4.3 | **EVS fixed-point v16 (TELEPHONY_USE_EVS_FX)** — WIP. Parent-only shim/source-list work now resolves the original typedef/cnst/stat_com compile blockers and builds the FX static libs plus `TelephonyDSP`, but `TelephonyRunner` still fails final link (`LNK1120: 138 unresolved external references`) because multiple FX helper families still need ports or deliberate stubs. See [CHANGELOG.md](CHANGELOG.md). | — WIP / link-blocked |
| 4.4 | **Wideband extension to narrowband transcoding artifacts** — tandem coding effects when WB input is encoded as NB, then decoded and re-encoded. | ★★★★ |
| 4.5 | **Voice activity detection (VAD-2) with hangover** — implement proper 3GPP-style VAD with primary decision, hangover addition, and burst-length smoothing. Currently only simple energy threshold. | ★★★ |

---

## Out of scope (explicitly excluded)

| Item | Reason |
|------|--------|
| Real network sockets (UDP/RTP send/receive) | DAW plugin, no external I/O |
| SIP / SDP / IMS signaling stack | Not a real phone; pure audio processor |
| Multi-party conferencing / N-way mixing | VST insert effect on a single track |
| SRTP / DTLS encryption | No network layer to protect |
| External microphone / speaker device I/O | Host DAW handles audio I/O via ASIO/WASAPI |
| Real-time VoIP client mode | Offline deterministic rendering for reproducibility |
| Full RAN stacks (srsRAN, OAI, Osmo-*) | GPL/AGPL license incompatibility |
| PESQ / POLQA perceptual scoring | Licensing cost; not needed for plugin |
