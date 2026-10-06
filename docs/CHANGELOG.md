# Changelog / 作業履歴

作業履歴の詳細ログです。新しいものが上。技術的な正確さを保つため、
既存エントリは記録された当時の英語のまま残しています。

---

## 2026-10-06 — End-to-end build/run verification, realtime-safety and FX fixes

All four configurations were built and exercised on this machine
(MSVC 14.51.36231 / VS 18, CMake 4.3.1, Ninja 1.13.2). `x64-release`,
`x64-release-dist` and `x64-release-jbm` build and pass ctest
(`dsp_smoke` 3/3, plus `evs_jbm_smoke` in the JBM tree).

* **EVS fixed-point NULL dereference fixed** (`evs_api_fx.c`): the FX
  `Decoder_State_fx` declares `UWord16 *bit_stream_fx` as a *pointer*
  (`lib_dec/stat_dec_fx.h`) where the float variant embeds an array, and
  nothing in `external/3gpp-evs` allocates it - `read_indices_fx()` writes the
  unpacked G.192 bits through it (`lib_com/bitstream_fx.c`, G.192 GOOD-frame
  branch).  With a `calloc`'d state that pointer was NULL, so the first decoded
  frame died with `0xC0000005`.  `evs_dec_create()` now allocates
  `MAX_BITS_PER_FRAME+16` entries and `evs_dec_destroy()` frees them.  This
  restores the fix described in `EVS_FX_PROGRESS.md` §6.3 that was lost in the
  later `_fx` field-rename rewrite.
* **EVS fixed-point digital-silence guard restored** (`evs_api_fx.c`): the
  basic-op `div_s()` implementation calls `abort()` on a zero denominator, and
  an all-zero frame is what drives the Levinson-Durbin / gain recursion into
  it.  `evs_enc_process()` now detects an all-zero frame, skips `evs_enc_fx()`
  and lets the serialiser emit a zero-length G.192 frame (the standard "no
  data" marker the decoder maps onto DTX / SP_LOST) - the same lost fix the
  progress notes describe.
* **FX decoder concealment path aligned with the float wrapper**: `bfi_fx` now
  selects `FRAMEMODE_MISSING` instead of always running `FRAMEMODE_NORMAL`.
* **OPUS_VOIP flush tail bounded** (`TelephonyRunner.cpp`): the smart-flush
  loop stopped on a -54 dBFS threshold, which DTX/CNG codecs never cross -
  Opus kept emitting comfort noise and the loop ran to its 500-block safety
  limit, appending ~10.7 s of noise to a 2.7 s file (13.39 s output).  The cap
  is now expressed in time (1.0 s) and the same file renders 3.73 s.  All
  other modes are byte-identical to their previous output.
* **Audio-thread allocations removed** (`dsp/SignalProcessor.{h,cpp}`):
  `process()` allocated `exchange`, `processedChannels`, `d` and `w` vectors on
  every block.  They are now preallocated members grown on demand by
  `ensureScratch()`, and the partially-written regions are explicitly cleared
  so the reused buffers keep the old zero-fill semantics.
* **Artifact noise made deterministic** (`dsp/ChannelProcessor.{h,cpp}`):
  `applyArtifacts()` used a function-local `static` seed shared by every
  instance plus `std::rand()` for the GSM clicks, so artifact output depended
  on instance count, processing order and the CRT's global rand() state.  It
  now uses a per-instance LCG (`artifactSeed`, re-seeded in `reset()`).
* **G.711 codec no longer rebuilt on unchanged law**
  (`dsp/ChannelProcessor.cpp`): `setG711Law()` is called for both legs on every
  parameter update via `applyRouteToChannels()`, and it unconditionally called
  `recreateCodec()` while PSTN_G711 was selected - i.e. every host knob move
  re-created the codec and its resampler/PLC state.
* **Uninitialised members fixed** (`dsp/ChannelProcessor.cpp`): `evsG711Law`,
  `evsScVbrEnabled`, `opusBandwidth` and `amrNbMode` were missing from the
  constructor initialiser list and were read uninitialised.
* **Latency constant single-sourced** (`dsp/SignalProcessor.cpp`):
  `updateLatency()` now derives from `LATENCY_MS` (`dsp/Types.h`) instead of a
  duplicated literal `0.1`.
* **Docs corrected**: README no longer claims the FX variant is link-blocked
  (`TelephonyRunner.exe` links with `LNK1120 = 0`) and documents the real
  VST3 module path (`VST3/TelephonyVoice.vst3`; the `VST3/Release/...` bundle
  holds only resources); CMakeLists' test-skip comment and ROADMAP 4.3 now
  describe the runtime situation instead of a link failure.

**Still open (verified, not fixed):** the FX core port itself.  With the
wrapper bugs above fixed, a mono FX render now completes `init_encoder_fx`,
`init_decoder_fx`, roughly 30 frames of encode/decode and then dies with
`0xC0000409` (stack-buffer overrun) inside `evs_enc_fx`; a stereo render dies
earlier with `0xC0000005` in the second `init_decoder_fx`.  Both point at
memory corruption inside the stub-implemented core families (ACELP/TCX coding,
FD-CNG, LPD state machine) that ROADMAP 4.3 tracks, not at the wrapper.
`evs_api_fx.c` also still round-trips its G.192 bitstream through a `tmpfile()`
per frame (with a fixed-name `evs_tandem.192` fallback in the CWD), unlike the
float wrapper which was converted to fully in-memory serialisation.


## 2026-07-03 — Bypass latency alignment, CI, README restructure

* **Latency-compensated bypass** (`TelephonyVoice.cpp/h`): the bypass path
  used to pass input through with zero delay while the plugin reports
  `getLatencySamples()` to the host, so toggling bypass shifted the audio
  in time. Bypass now routes the dry signal through a delay line of the
  same length. The ring is fed on every `process()` call (read only while
  bypassed) so a mid-playback toggle stays seamless. This also fixed a
  latent channel bug: the old bypass loop iterated `data.numOutputs`
  (number of output *buses*, i.e. 1) instead of the output bus's channel
  count, so stereo bypass only copied the left channel.
* **GitHub Actions CI** (`.github/workflows/ci.yml`): Windows matrix over
  the `x64-release`, `x64-release-dist`, and `x64-release-jbm` presets;
  each job configures, builds, and runs ctest.
* **CMake presets added**: `x64-release-dist`
  (`TELEPHONY_DISTRIBUTION_BUILD=ON`) and `x64-release-jbm`
  (`TELEPHONY_USE_EVS_JBM=ON`) so the CI and local builds share the same
  configuration entry points. `EVSJbmSmoke` is now registered as a ctest
  (`evs_jbm_smoke`) in JBM builds.
* **README restructured**: rewritten in Japanese; the work-history log
  moved to this file and the implementation roadmap to
  [`ROADMAP.md`](ROADMAP.md).

## 2026-07-03 — VST wiring fixes, DSP bug fixes, in-memory EVS API, tests

* **Biquad "disabled = mute" bug fixed** (`dsp/Biquad.h`): `Biquad::reset()`
  zeroed all coefficients, so any mode that used `reset()` to mean "no
  filtering" ran its audio through an all-zero filter. This silenced
  `EVS_NATIVE` and `OPUS_VOIP` entirely at zero network degradation (both
  skip the band cascade in that case). `reset()` is now an identity
  pass-through. Caught by the new `TelephonyDspSmoke` test.
* **Opus jitter-queue playback logic fixed** (`dsp/OpusCodec.cpp`): the
  receive side consumed each packet immediately after encode via the FEC
  path (2 frames before its scheduled arrival), so the decoder never saw
  real payload and output only concealment. Playback now tracks the
  correct slot (`playbackFrame - basePlaybackDelay()`), decodes the exact
  packet when it has arrived, uses next-packet FEC only for genuinely
  late/lost slots (without consuming the next packet), and drops stale
  packets. Jitter is now zero at degradation 0 (direct-feed behaviour).
* **VST parameter wiring completed** (`TelephonyVoice.cpp`): `process()`
  previously ignored `kParamG711Law`, `kParamAmrWbMode`, `kParamOpusBitrate`,
  `kParamEvsDtxSidInterval`, and `kParamEvsScVbr`; the controller never
  registered AMR-WB Mode / Opus Bitrate / EVS SC-VBR at all. All five are
  now handled in `process()` and registered in the controller UI. The EVS
  DTX SID interval `RangeParameter` had `stepCount=1` (only 0 or 100
  selectable); it is now 100 steps.
* **State persistence completed**: `getState()`/`setState()` now persist
  `currentAmrWbMode`, `currentG711Law`, `currentEvsScVbr`, and
  `currentOpusBitrate` (appended after the existing fields, so older saved
  states still load with defaults). `setComponentState()` was an empty stub
  — the UI showed defaults after project reload; it now mirrors the
  processor stream and restores every parameter position.
* **Plugin identity fixed**: the factory definition still carried the
  Voxengo placeholder vendor/URL/email; it now identifies this project
  (vendor `Rumia Channel`). The placeholder FUIDs were replaced with
  freshly generated ones. **Note:** hosts identify plugins by FUID, so
  DAW projects saved with earlier dev builds will not find the plugin
  under the new IDs. The IDs must stay stable from now on.
* **`evs_api.c` no longer performs file I/O** — the G.192 round-trip
  through `tmpfile()` (unsafe on the audio thread; broken for non-admin
  users on Windows, where the fallback also opened a *shared fixed-name*
  file in the CWD, corrupting concurrent instances) is gone. The encoder
  serialises `ind_list` into the caller's buffer directly (replicating the
  reference `write_indices()` G192 branch), and the decoder repacks the
  G.192 words into a compact AU and feeds the exported
  `read_indices_from_djb()`. The decoder also reuses a preallocated PCM
  buffer instead of a per-frame `calloc`, mirrors the reference decoder
  main loop by running `FRAMEMODE_MISSING` concealment when the RX DTX
  handler flags an untransmitted gap, and validates the G.192 header
  (sync word, frame length, rate allowlist) instead of `exit(-1)`.
* **Smoke tests added** (`tests/TelephonyDspSmoke.cpp`, ctest name
  `dsp_smoke`): sweeps every `EraMode` reachable in the build config over
  a speech-like burst signal (a steady sine is classified as background
  noise by the EVS VAD / energy VAD and would read as silence) and checks
  finite, non-silent output; round-trips the raw EVS C API at WB 16 kHz
  and SWB 32 kHz (the plugin default) with DTX on, covering SID/NO_DATA
  frames and the PLC path. Run with `ctest --test-dir out/build/x64-release`.

## Tier 0 — codec control points (`b354da6`)

* **G.711 A-law selection**: `G711Codec` now supports A-law in addition to µ-law.
  VST UI (`kParamG711Law`) + CLI (`--g711-law ulaw|alaw`).
* **AMR-NB 8 modes** (MR475~MR122 / 4.75~12.2 kbps): `AMRNBCodec` ctor accepts
  `mode` parameter. VST (`kParamAmrNbMode`) + CLI (`--amr-nb-mode 0..7`).
* **AMR-WB 9 modes** (6.60~23.85 kbps): same pattern. VST (`kParamAmrWbMode`)
  + CLI (`--amr-wb-mode 0..8`).
* **Opus variable bitrate** (6~256 kbps): `OpusCodec::setBitrate()`. VST
  (`kParamOpusBitrate`, 12 choices) + CLI (`--opus-bitrate`).
* **Opus bandwidth switching** (NB/MB/WB/SWB/FB): `OpusCodec::setMaxBandwidth()`.
  VST (`kParamOpusBandwidth`, 5 choices) + CLI (`--opus-bw`).
* **EVS DTX SID interval**: `EVSCodec`/`EVSCodecJbm::setDtxSidInterval()`
  (0=variable, 3~100=fixed frames). VST (`kParamEvsDtxSidInterval`) +
  CLI (`--evs-dtx-sid-interval N`).
* **EVS SC-VBR** (Source-Controlled VBR, 5.9 kbps mode):
  `EVSCodec`/`EVSCodecJbm::setScVbrEnabled()`. VST (`kParamEvsScVbr`) +
  CLI (`--evs-sc-vbr`).
* **File structure refactor**: `TelephonyDSP.h` (744 lines) + `TelephonyDSP.cpp`
  (2002 lines) split into `dsp/` subdirectory with 27 per-class files.

## Earlier work

* `external/3gpp-evs` added as a submodule from
  [`lem21h/3gpp-evs`](https://github.com/lem21h/3gpp-evs) (2024-05 3GPP EVS
  update; the older wanglihe/3gpp-evs was tried first and removed).
* `cmake/3gpp-evs.cmake` builds three static libs from the float variant:
  `evs-lib-com`, `evs-lib-enc`, `evs-lib-dec`.  Plus an `INTERFACE
  /FORCE:MULTIPLE` link option (MSVC) to resolve basop symbol collisions with
  `opencore-amrnb`.
* `evs_api.h` / `evs_api.c` – clean C wrapper around the reference
  `init_encoder` / `evs_enc` / `init_decoder` / `evs_dec` quartet.
  Originally staged the G.192 bitstream through `tmpfile()`; now fully
  in-memory (see the 2026-07-03 entry above).
* `evs_api_fx.c` – fixed-point wrapper for `TELEPHONY_USE_EVS_FX=ON`.
  This path is now partially wired, but remains experimental and link-blocked
  (see the fixed-point status section below).
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

## EVS Native Integration Status

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
* Fixed-point EVS (`TELEPHONY_USE_EVS_FX`) has parent-repo-only WIP wiring.
  It configures and compiles much further than before, but is not complete:
  `TelephonyRunner` still fails at final link with unresolved FX helpers.
  See the next section.

## EVS Fixed-Point (`TELEPHONY_USE_EVS_FX`) — WIP / link-blocked

The fixed-point EVS variant (TS 26.442 v16.4.0) is gated by
`TELEPHONY_USE_EVS_FX=ON`. This is **not a shippable path yet**; the float
EVS wrapper remains the practical supported EVS implementation. The current
FX work keeps `external/3gpp-evs` read-only and applies compatibility shims
from the parent repository only.

Latest checked command:

```powershell
cmake --preset x64-release -DTELEPHONY_USE_EVS_FX=ON -DTELEPHONY_USE_EVS_JBM=OFF -DTELEPHONY_EXPERIMENTAL_NETWORK=OFF
cmake --build out/build/x64-release --target TelephonyRunner --parallel
```

**Current result:** configure succeeds; `evs-lib-com-fx`, `evs-lib-enc-fx`,
`evs-lib-dec-fx`, and `TelephonyDSP` build. The `TelephonyRunner` build now
gets through all object/static-library compilation, but the final executable
link still fails on MSVC with `LNK1120: 138 unresolved external references`.

**Progress made:**

1. `evs_api_fx.c` rewritten as a real wrapper around the fixed-point APIs
   (`init_encoder_fx`, `evs_enc_fx`, `init_decoder_fx`, `evs_dec_fx`,
   `destroy_encoder_fx`, `destroy_decoder`). Signature-compatible with
   `evs_api.c`, so `dsp/EvsCodec` needs zero changes.
2. `cmake/3gpp-evs.cmake` now generates and force-includes an FX compatibility
   shim that suppresses the float `typedef.h` / `cnst.h` collisions, includes
   the fixed-point `basic_op/typedefs.h` + `cnst_fx.h`, remaps the shared
   `stat_com.h` types that FX sources need, fixes the `Mpy_32_16` 2-arg vs
   3-arg collision, and overrides broken no-WMOPS control-flow macros.
3. The parent CMake adds the fixed-point helper sources needed to get through
   compilation: `basic_op` helpers, `basic_math` including `math_32.c`,
   `basop_mpy.c`, `basop_com_lpc.c`, `basop_lsf_tools.c`, `basop_util.c`, and
   `rom_basop_util.c`. MSVC `/FORCE:MULTIPLE` is used for expected duplicate
   basic-op symbols shared with the AMR libraries.
4. `external/3gpp-evs` remains untouched; all changes are parent-repo CMake / shim
   changes.

**Known exclusions / blockers:**

* `basop_tcx_utils.c` remains excluded. It calls `BASOP_cfft` with a 4-argument
  form, while the FX declaration uses a 6-argument form, and it also pulls in
  float `prot.h` / `cnst.h` / `rom_com.h` headers.
* `lag_wind.c` remains excluded. The upstream file is float-only and triggers
  float `stat_dec.h` / `prot.h` / `rom_com.h` / `cnst.h` header leakage; FX
  callers still need fixed-point `lag_wind` / `adapt_lag_wind` implementations
  or a deliberate port.
* The remaining unresolved externals are not one missing library. They are
  missing fixed-point implementations/ports or float-only helper families:
  TNS, CLDFB, FD-CNG, TCX/TEC/TBE, pitch/ACELP, post-filter/concealment,
  `lag_wind` / `hp20` / `lerp` / `get_gain`, and `BASOP_cfft` / FFT / divide
  helpers. Continuing requires a broader architecture decision: port these FX
  helpers in the parent repository, introduce controlled stubs/wrappers, or keep
  FX disabled while the float EVS path remains the supported path.

## Experimental / Non-GPL Additions (SpeexDSP + Opus)

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
  `lastFrameIsSpeech()` returns `lastEnergyVadProb > 0.5` instead of
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
