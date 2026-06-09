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
* The distribution build (`TELEPHONY_DISTRIBUTION_BUILD=ON`) still
  aliases `EVS_NATIVE` to `EVS_LIKE` and omits it from the runner's
  mode list, so the EVS reference is never linked or invoked in shipped
  builds.
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
│   ├── r8brain.cmake
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
    ├── r8brain/           # sample-rate converter
    ├── vo-amrwbenc/       # AMR-WB encoder
    └── vst3sdk/           # Steinberg VST3 SDK
```

## Build Options

| Option                     | Default | Effect                                                                 |
| -------------------------- | ------- | ---------------------------------------------------------------------- |
| `TELEPHONY_USE_EVS_FX`     | OFF     | Build fixed-point EVS (TS 26.442) instead of float (TS 26.443).         |
| `TELEPHONY_DISTRIBUTION_BUILD` | OFF | Strip AMR/AMR-WB/EVS references and expose only distributable modes. |

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
