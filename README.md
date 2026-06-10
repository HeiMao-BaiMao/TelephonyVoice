# TelephonyVoice

VST3 plugin (and CLI runner) that emulates telephone / cellular voice paths.
The VST UI uses route labels such as `in -> exchange -> out`, `fixed line`,
`4G mobile`, and `5G mobile` rather than codec-standard names. Personal-use
builds can use the bundled 3GPP reference implementations; distribution builds
hide those modes and use distributable stand-ins only.

## Quick Start

```pwsh
# 1. Clone with submodules
git clone --recurse-submodules <this-repo> TelephonyVoice
cd TelephonyVoice

# 2. Configure (VS 18 / Ninja)
cmd /c "call `"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`" && cmake --preset x64-release"

# 3. Build
cmd /c "call `"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`" && cmake --build out/build/x64-release --parallel"
```

Outputs:

* `out/build/x64-release/TelephonyRunner.exe` – CLI
* `out/build/x64-release/VST3/Release/TelephonyVoice.vst3` – plugin

Run the CLI on any 16-bit PCM WAV:

```pwsh
.\out\build\x64-release\TelephonyRunner.exe path\to\input.wav
```

It produces `<input>.<mode>.wav` for each internal mode in parallel.

## Routes and Modes

The VST path is modeled as two independently degraded legs:

```text
in -> exchange -> out
```

UI parameters:

* `in` – fixed line / 2G mobile / 3G mobile / 4G mobile / 5G mobile
* `out` – fixed line / 2G mobile / 3G mobile / 4G mobile / 5G mobile
* `degraded segment: in -> exchange -> out` – both legs, only input-to-exchange,
  only exchange-to-output, or none
* `packet loss` – applied as packet/frame erasure on the selected degraded leg(s)
* `network degradation` – narrows the simulated path bandwidth and increases burst
  loss on the selected degraded leg(s)

Internally those user-facing endpoints map to codec-era modes:

| `EraMode`                | Sample rate | Codec                  | Real implementation?                  |
| ------------------------ | ----------- | ---------------------- | ------------------------------------- |
| `PSTN_G711`              | 8 kHz       | G.711 µ-law            | ✅ (Public Domain, Sun)               |
| `GSM_FR`                 | 8 kHz       | GSM 06.10              | ✅ (libgsm, permissive)                |
| `AMR_NB_3G`              | 8 kHz       | AMR-NB MR122           | ✅ (opencore-amr, Apache 2.0)          |
| `AMR_WB_VOLTE`           | 16 kHz      | AMR-WB 12.65 kbps      | ✅ (vo-amrwbenc + opencore-amrwb)     |
| `EVS_LIKE`               | 32 kHz      | *filter-only stand-in* | ❌ (safe to ship)                     |
| `EVS_NATIVE`             | 8/16/32/48  | 3GPP EVS reference     | ✅ (3GPP TS 26.443 v12.7.0/v13.3.0)   |
| `Bypass`                 | host        | –                      | –                                     |
| `OPUS_VOIP` *(experimental)* | 48 kHz   | Opus (VOIP application, 24 kbps) | ✅ (opus, BSD) — non-distribution only |
| `EVS_JBM` *(experimental)*   | 8/16/32/48 | EVS + Stage-1 JBM/VoIP adapter | ✅ (3GPP EVS + `EvsRXlib`) — non-distribution, `TELEPHONY_USE_EVS_JBM=ON` only |

## Current State of Work

### Done

* `external/3gpp-evs` added as a submodule from
  [`lem21h/3gpp-evs`](https://github.com/lem21h/3gpp-evs) (2024-05 3GPP EVS
  update; the older wanglihe/3gpp-evs was tried first and removed).
* `cmake/3gpp-evs.cmake` builds three static libs from the float variant:
  `evs-lib-com`, `evs-lib-enc`, `evs-lib-dec`.  Plus an `INTERFACE
  /FORCE:MULTIPLE` link option (MSVC) to resolve basop symbol collisions with
  `opencore-amrnb`.
* `evs_api.h` / `evs_api.c` – clean C wrapper around the reference
  `init_encoder` / `evs_enc` / `init_decoder` / `evs_dec` quartet.  Uses
  `tmpfile()` to round-trip the G.192 bitstream between encoder and decoder.
* `evs_api_fx.c` – stub for the fixed-point variant (`TELEPHONY_USE_EVS_FX`
  build option).  Not implemented. The current FX source set still mixes
  float and fixed headers (`stat_com.h` pulls in `cnst.h` next to
  `cnst_fx.h`), so this cannot be made the default yet.
* `TelephonyDSP` extended with an `EVS_NATIVE` mode, `EVSCodec` class
  wrapping the C API, EVS configuration plumbing on `ChannelProcessor` and
  `SignalProcessor`.  Default config: SWB 32 kHz / 13.2 kbps.
* `TelephonyRunner` calls `setEVSConfig` for `EVS_NATIVE` and processes it
  alongside the other era modes (personal/normal build). The integration
  crash traced to two issues, both now resolved: EVS `wb_vad` symbol
  collision with `vo-amrwbenc` (fixed at the parent-repo level by passing
  `wb_vad=evs_wb_vad` / `wb_vad_init=evs_wb_vad_init` as compile
  definitions to the EVS lib targets in `cmake/3gpp-evs.cmake` so the EVS
  sources compile as if those symbols were renamed, without editing the
  submodule) and r8brain `CDSPResampler24` `aMaxInLen=1024` overflow when
  EVS-like/native 640-sample frames accumulated to >1024 samples (fixed
  by chunking resampler calls). Distribution build still aliases
  `EVS_NATIVE` to `EVS_LIKE` and omits it from the runner's mode list.
* VST3 route selection now exposes `in`, `out`, and
  `degraded segment: in -> exchange -> out`. Normal/personal builds expose
  fixed line, 2G, 3G, 4G, 5G, and 5G precise/native. Distribution builds expose
  only fixed line, 2G, and 5G stand-in.
* Packet loss is modeled before/during the codec frame rather than as output
  noise. AMR-NB and AMR-WB use the decoder `bfi` path. EVS Native uses
  `FRAMEMODE_MISSING`. G.711, GSM, and EVS-Like use a waveform-repetition PLC
  with pitch estimation and attenuation because those bundled APIs do not expose
  a standards-grade PLC entry point.
* `TELEPHONY_DISTRIBUTION_BUILD=ON` is implemented as a non-3GPP build path:
  AMR/AMR-WB/EVS reference libraries are not included or linked, the UI only
  exposes fixed line / 2G / 5G stand-in, and direct `EVS_NATIVE` requests are
  aliased to `EVS_LIKE`.
* `evs_api_rx.h` / `evs_api_rx.c` add a Stage-1 parent adapter around the 3GPP
  `EvsRXlib` (`EVS_RX_*`) for the JBM/VoIP receive path. It is **OFF by
  default** via the `TELEPHONY_USE_EVS_JBM` build option, float EVS only,
  non-distribution only. The adapter now exposes a public
  `evs_rx_jbm_g192_to_compact_au` helper, and a new `EVSCodecJbm` class
  (sibling of `EVSCodec`) wires the encoder and the JBM together as a
  single `ICodec`. The `EVS_JBM` mode is now reachable from
  `TelephonyRunner` (suffix `evs_jbm`) when `TELEPHONY_USE_EVS_JBM=ON`,
  gated by the same CMake option. The mode is **not** exposed in the VST
  UI. `EVSCodecJbm` now also runs a small deterministic
  packet-arrival / jitter queue between the encoder and the JBM: each
  encoded AU is stamped with a `recvMs = rtpTsMs + offset` where the
  offset comes from an LCG seeded from
  `configureNetwork(networkDegradation)`; queue cap is 16, jitter window
  scales linearly with `networkDegradation` (0 => no jitter, preserving
  the previous direct-feed behaviour), and the queue is fed into the JBM
  with its deterministic recv time exactly as the adapter's per-packet
  wall clock. Offline renders remain deterministic (no real wall clock is
  read), and no encoder CTLs are issued yet — only the queue +
  `configureNetwork` plumbing. Still **not** exposed in the VST UI.
  * Raw AU contract: callers feed compact EVS access unit bytes
    (the same bit-packed payload the encoder produces); `au_bits_count` is
    in **bits** (not bytes) and must already be aligned to the codec frame
    size; RTP timestamps from the wire must be converted to the JBM's 1 ms
    timeline before being fed into the receive adapter.
  * When `TELEPHONY_USE_EVS_JBM=ON`, CMake also builds `EVSJbmSmoke`. It
    validates the G.192 short-stream to compact MSB-first AU conversion through
    `evs_rx_jbm_feed_frame` / `evs_rx_jbm_get_samples`, drains with
    `evs_rx_jbm_is_empty` only at end-of-stream, checks non-zero decoded
    energy and `evs_rx_jbm_get_fec_offset`, and uses only public parent APIs.
* AMR-NB and AMR-WB encoders now enable their bundled 3GPP DTX/CNG path by
  default in normal / personal builds. `AMRNBCodec` initializes
  `Encoder_Interface_init(dtxEnabled ? 1 : 0)` (default `dtxEnabled = true`)
  and `AMRWBCodec` passes `dtxEnabled ? 1 : 0` as the final `E_IF_encode`
  argument (the AMR-WB init API does not take DTX). When DTX is enabled,
  the encoded AMR frame payload size becomes SID/data-dependent inside the
  3GPP reference encoder (it no longer matches a fixed per-mode byte count).
  Both classes expose `setDtxEnabled(bool)` / `isDtxEnabled()` for callers
  that need to override the default; the `EVS_NATIVE` DTX behaviour added
  in the previous bullet is unchanged. The distribution build keeps its
  pass-through AMR stubs with identical constructor signatures and does
  not invoke any external encoder API, so it is unaffected.

### EVS Native Integration Status

**Status: the `EVS_NATIVE` integration crash that previously blocked the
runner is resolved.** The standalone EVS API (`evs_api.c` over the
`init_encoder` / `evs_enc` / `init_decoder` / `evs_dec` quartet) was
working throughout; the `SignalProcessor` / `TelephonyRunner`
integration was crashing because of two upstream issues that have both
been fixed in this tree:

1. **EVS `wb_vad` symbol collision with `vo-amrwbenc`.** Both libraries
   export a `wb_vad` symbol; under `INTERFACE /FORCE:MULTIPLE` (MSVC) one
   wins at link time and the other runs with the wrong implementation,
   which corrupts encoder state. Resolved at the parent-repo level by
   passing `wb_vad=evs_wb_vad` and `wb_vad_init=evs_wb_vad_init` as
   `target_compile_definitions` to `evs-lib-com` / `evs-lib-enc` /
   `evs-lib-dec` in `cmake/3gpp-evs.cmake`. The C preprocessor rewrites
   the identifiers in the EVS headers, sources, and call sites so the
   binary exposes `evs_wb_vad*` and `vo-amrwbenc` keeps its own
   `wb_vad*`. The submodule is not modified.
2. **r8brain `CDSPResampler24` `aMaxInLen=1024` overflow.** EVS-like and
   EVS-native paths emit 640-sample frames; accumulating more than 1024
   samples before a resampler call exceeded the resampler's per-call
   input cap. Resolved by chunking resampler calls so each call stays
   within the supported input length.

**Current state:**

* The normal/personal build's `TelephonyRunner` now includes
  `EVS_NATIVE` in its mode list and produces an `evs_native` output WAV
  end-to-end.
* `EVS_NATIVE` (non-distribution) now exercises the 3GPP EVS internal
  VAD/DTX/SID/CNG path by default. `EVSCodec` calls
  `evs_enc_create_ex` with DTX enabled and a variable SID update
  interval (`dtx_sid_interval=0`); the legacy `evs_enc_create` entry
  point is preserved as a thin wrapper that maps to `_ex(..., NULL)`
  and reproduces the historical "DTX off" behaviour. Channel-aware
  mode (RF) and source-controlled VBR (SC-VBR) stay off; RF is
  restricted by the EVS spec to 13.2 kbps with >= 16 kHz input, and
  JBM/RTP-packet-loss handling remain future work.
* The distribution build (`TELEPHONY_DISTRIBUTION_BUILD=ON`) still
  aliases `EVS_NATIVE` to `EVS_LIKE` and omits it from the runner's
  mode list, so the EVS reference is never linked or invoked in shipped
  builds. The DTX/CNG wiring in `EVSCodec` is also compiled out under
  `TELEPHONY_DISTRIBUTION_BUILD`, so the distribution path is
  unaffected.
* Fixed-point EVS (`TELEPHONY_USE_EVS_FX`) remains unimplemented and
  untested; see `### CMake Build Option (Untested Path)` below.

### CMake Build Option (Untested Path)

```pwsh
cmake -DTELEPHONY_USE_EVS_FX=ON --preset x64-release
```

This is intended to build the fixed-point variant (TS 26.442 v16.4.0), but it
is not currently usable. The float wrapper (`evs_api.c`) is replaced by the
stub `evs_api_fx.c`, which returns errors, and the FX library build currently
fails before linking because fixed-point sources include `cnst_fx.h` while
`stat_com.h` still pulls in the float `cnst.h`.

The user-facing target is that `5G mobile (precise)` / `EVS_NATIVE` should use
the fixed-point implementation by default, matching real mobile deployments.
Do not flip `TELEPHONY_USE_EVS_FX` to ON by default until:

* the fixed-point source collection no longer mixes float and FX headers;
* `evs_api_fx.c` implements `evs_enc_create`, `evs_enc_process`,
  `evs_dec_process`, and `evs_dec_process_lost`;
* the normal and distribution builds both pass, with distribution still hiding
  all native 3GPP codecs.

## Distribution / Non-EVS Edition

User requirement: a separate, distributable build that drops
AMR/AMR-WB/EVS entirely (the codecs that have patent / 3GPP-member
encumbrance) and only ships fixed line, 2G, and the 5G stand-in route.

Implemented path:

* `TELEPHONY_DISTRIBUTION_BUILD=ON` excludes
  `cmake/vo-amrwbenc.cmake`, `cmake/opencore-amr.cmake`, and
  `cmake/3gpp-evs.cmake`.
* `TelephonyDSP` compiles without AMR/AMR-WB/EVS symbols. AMR stand-ins keep
  the existing resampling/filter path and pass frames through instead of using
  the 3GPP codecs.
* `EraMode::EVS_NATIVE` is aliased to `EVS_LIKE` under the distribution build.
* The VST3 UI lists only fixed line, 2G mobile, and 5G mobile stand-in under
  the distribution build. 3G, 4G, and 5G precise/native are not reachable from
  the distribution UI.
* `TelephonyRunner` lists only G.711, GSM, and EVS-Like under the distribution
  build because it still uses the internal mode names for generated filenames.

## Code Layout

```
.
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
│   ├── 3gpp-evs.cmake     # builds evs-lib-{com,enc,dec}[-fx]
│   ├── g711.cmake
│   ├── libgsm.cmake
│   ├── opencore-amr.cmake
│   ├── opus.cmake         # add_subdirectory(external/opus) with programs/tests OFF
│   ├── r8brain.cmake
│   ├── speexdsp.cmake     # static lib from selected libspeexdsp sources
│   └── vo-amrwbenc.cmake
├── evs_api.h              # public C API for the 3GPP EVS wrapper
├── evs_api.c              # float variant (working wrapper)
├── evs_api_fx.c           # fixed-point variant stub
├── TelephonyDSP.h         # public C++ API of the route-aware SignalProcessor
├── TelephonyDSP.cpp       # two-leg path, codec emulations, PLC, resampler/filter chain
├── TelephonyVoice.h       # VST3 processor + edit controller
├── TelephonyVoice.cpp     # VST3 glue
├── TelephonyRunner.cpp    # CLI: applies every era to a WAV
└── external/
    ├── 3gpp-evs/          # 3GPP EVS reference (TS 26.443 + 26.442)
    ├── G711_G72x/         # G.711/G.721/G.723
    ├── libgsm/            # GSM 06.10
    ├── opencore-amr/      # AMR-NB + AMR-WB decoder
    ├── opus/              # Opus codec (BSD)
    ├── r8brain/           # sample-rate converter
    ├── speexdsp/          # SpeexDSP (BSD, only preprocess/jitter/FFT subset built)
    ├── vo-amrwbenc/       # AMR-WB encoder
    └── vst3sdk/           # Steinberg VST3 SDK
```

## Build Options

| Option                     | Default | Effect                                                                 |
| -------------------------- | ------- | ---------------------------------------------------------------------- |
| `TELEPHONY_USE_EVS_FX`     | OFF     | Build fixed-point EVS (TS 26.442) instead of float (TS 26.443).         |
| `TELEPHONY_USE_EVS_JBM`    | OFF     | Build the experimental EVS Stage-1 JBM/VoIP receive adapter (`evs_api_rx`) around 3GPP `EvsRXlib` plus the `EVSJbmSmoke` smoke executable. Incompatible with `TELEPHONY_DISTRIBUTION_BUILD` and with `TELEPHONY_USE_EVS_FX`. |
| `TELEPHONY_DISTRIBUTION_BUILD` | OFF | Strip AMR/AMR-WB/EVS references and expose only distributable modes. |
| `TELEPHONY_EXPERIMENTAL_NETWORK` | ON (forced OFF in distribution builds) | Build SpeexDSP + Opus and expose `OPUS_VOIP` mode (BSD-licensed). |

## License Note

For personal use, all libraries are permissively licensed
(Apache 2.0 / MIT / public domain / custom permissive).  However,
**AMR/AMR-WB/EVS codec patents** (VoiceAge, Fraunhofer, NTT DoCoMo,
Ericsson, Nokia, etc.) apply to commercial distribution.  Use
`TELEPHONY_DISTRIBUTION_BUILD=ON` for a build that avoids those codec
implementations.

The normal/personal build exposes `5G mobile (precise)` / `EVS Native` in the
VST3 UI for testing. It must not be shipped. The distribution build keeps
`EVS_NATIVE` aliased to `EVS_LIKE` and out of the runner's mode list, so
the EVS reference is never linked or invoked in shipped builds.

## Experimental / Non-GPL Additions

This first experimental step adds two BSD-licensed libraries — SpeexDSP and
Opus — to the personal / non-distribution build, behind a new CMake option
`TELEPHONY_EXPERIMENTAL_NETWORK` (default ON, **always OFF** under
`TELEPHONY_DISTRIBUTION_BUILD`).

The goal is to start reproducing 2G / 3G / 4G / 5G voice behaviour more
faithfully while staying clear of GPL / AGPL and the heavier 3GPP patent
encumbrances. Full RAN stacks (e.g. srsRAN, OAI) are intentionally **not**
included; we only use the BSD DSP / codec primitives that already ship in
those projects.

What is wired up in this step:

* `cmake/speexdsp.cmake` builds a small static `speexdsp-core` library from
  only the SpeexDSP preprocess / jitter / FFT sources we need
  (`preprocess.c`, `jitter.c`, `buffer.c`, `fftwrap.c`, `filterbank.c`,
  `kiss_fft.c`, `kiss_fftr.c`, `smallft.c`). The submodule is read-only —
  no file under `external/speexdsp/` is modified. The SpeexDSP
  autotools-generated `speexdsp_config_types.h` is replaced by a minimal
  C99 stdint-based shim that the cmake module writes into the build tree.
* `cmake/opus.cmake` pulls in `external/opus/CMakeLists.txt` via
  `add_subdirectory()` with `OPUS_BUILD_TESTING=OFF`,
  `OPUS_BUILD_PROGRAMS=OFF`, `OPUS_INSTALL_PKG_CONFIG_MODULE=OFF`,
  `OPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF`, and
  `OPUS_BUILD_SHARED_LIBRARY=OFF` so we only link the static `opus`
  library and do not build test programs / demos / docs.
* New `EraMode::OPUS_VOIP` mode. Numeric values for existing modes stay
  stable (the new mode is appended after `Bypass`). The CLI runner adds it
  to its normal mode list with the suffix `opus_voip`. The distribution
  runner does **not** include it.
* `TelephonyDSP::OpusCodec` wraps the libopus C API. Defaults: 48 kHz,
  20 ms frames, `OPUS_APPLICATION_VOIP`, 24 kbps target bitrate,
  complexity 6. Frame loss is handled by calling `opus_decode(NULL, 0)`,
  which lets Opus run its built-in PLC; the waveform concealer is only
  the last-resort fallback. The codec now models a tiny packetized
  transport on top of the bare encode/decode pair: in-band FEC is enabled
  via `OPUS_SET_INBAND_FEC(1)`, expected loss via
  `OPUS_SET_PACKET_LOSS_PERC(...)` driven by the clamped loss /
  degradation values, and a deterministic LCG-based jitter buffer
  simulates packet arrivals with a base + degradation-dependent delay.
  Missing packets are concealed by in-band FEC (`decode_fec=1`) when the
  next packet is available, otherwise by Opus's built-in PLC; DTX stays
  enabled via Opus's internal VAD.
* `TelephonyDSP::SpeexDSPAux` wraps `speex_preprocess` so the denoise (and
   in the future AGC) primitives can be reached from `ChannelProcessor`,
 and now also provides a local energy-based VAD that drives
 `getSpeechProbability()` / `lastFrameIsSpeech()`. The energy VAD is
 now actively wired into the DTX path for the `OPUS_VOIP` and
 `EVS_LIKE` modes: in `ChannelProcessor::processCodec` the helper is
 fed each freshly-quantized int16 frame just before dispatch, and a
 silence classification forces the effective `packetLost` flag to
 `true` so Opus emits its built-in CNG via DTX and `EVS_LIKE`'s
 simulated-path PLC fills the slot with comfort noise. EVS_NATIVE /
 EVS_JBM keep their own 3GPP VAD and are intentionally not touched;
 AMR / G.711 / GSM are not in the experimental network scope and are
 unaffected. Only `SPEEX_PREPROCESS_SET_DENOISE` is enabled on the
 Speex state; the explicit `SPEEX_PREPROCESS_SET_VAD` ctl is left
 commented out because SpeexDSP's VAD is still a placeholder that
 prints `The VAD has been replaced by a hack pending a complete
 rewrite` every time it is enabled. To still give callers a meaningful
 speech probability, `SpeexDSPAux` runs a simple RMS-based VAD on the
 post-denoise frame in `runPreprocess()` and caches the result;
 `configure()` auto-scales the threshold by `sqrt(frameSize/160)` so
 longer frames aren't penalized, and `setEnergyVadEnabled()` lets
 callers opt out or override the threshold. The energy VAD is on by
 default, so `getSpeechProbability()` returns a value in [0,1] and
 `lastFrameIsSpeech()` returns `lastEnergyVadProb >0.5` instead of
 the legacy `-1.0f / false` "unknown" sentinel. A new
 `ChannelProcessor::getLastVadProb()` getter exposes the same value
 (or `-1.0f` when the helper isn't present) for telemetry / UI
 wiring.

VST3 UI: the `OPUS_VOIP` mode is intentionally **not** exposed as a new
endpoint in this step, to avoid changing the existing route string list
or the host-side state-streams. It is reachable through `TelephonyRunner`
for now; adding an endpoint option is a small follow-up.

Distribution build (`TELEPHONY_DISTRIBUTION_BUILD=ON`):

* Forces `TELEPHONY_EXPERIMENTAL_NETWORK=OFF`.
* Skips `cmake/speexdsp.cmake` and `cmake/opus.cmake` entirely.
* `OpusCodec` / `SpeexDSPAux` fall back to no-op stubs compiled with the
  rest of `TelephonyDSP`, so the symbol table of a shipped plugin never
  references `opus_*` or `speex_*`.

What is deliberately still **not** in scope:

* Full 2G/3G/4G PHY / RAN stacks. Anything under
  `osmo-*` / `srsRAN` / `OAI` is GPL or AGPL and would force the whole
  project onto those licenses; we are not pulling those in.
* SpeexDSP-driven DTX for the legacy codecs in the non-experimental
  path (AMR / G.711 / GSM) and for the 3GPP-native EVS family
  (`EVS_NATIVE` / `EVS_JBM`). Those either already have their own
  VAD (EVS) or are out of the experimental network scope
  (AMR / G.711 / GSM); the energy VAD currently only drives DTX for
  `OPUS_VOIP` and `EVS_LIKE` (see the SpeexDSPAux bullet above).

## Open Questions for the Next Agent

1. Finish fixed-point EVS and make `EVS_NATIVE` default to it. This requires
   fixing the current FX header collision (`cnst_fx.h` plus float `cnst.h`) and
   replacing the `evs_api_fx.c` stub with a real wrapper.
2. Should the `EVS_LIKE` mode be promoted to a proper `EVS_DIST`
   filter-only class for the distribution build, or stay as a single
   mode in `SignalProcessor`?
3. Should a dedicated VSTGUI editor be added for a visual route diagram? The
   current implementation exposes `in`, `out`, and `degraded segment` through
   the host's generic VST parameter UI.
4. Are there other 3GPP EVS mirrors worth considering if the lem21h
   one stays unmaintained?  (AOSP branches, etc.)
