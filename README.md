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
* `TelephonyRunner` was wired up to call `setEVSConfig` for `EVS_NATIVE`.
  After discovering the hang (below) the iteration was commented out so the
  CLI ships in a usable state.
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

### Not Working Yet (Handoff)

**`EVS_NATIVE` hangs in `init_encoder` when called from `TelephonyRunner`
with default config (32 kHz / 13.2 kbps / SWB).**  The same configuration
works in the standalone test (`test_evs_direct.cpp`, since deleted) and
the issue is therefore in the `SignalProcessor` / `ChannelProcessor`
integration, not in the EVS library itself.

What is known:

* `evs_enc_create(32000, 13200, EVS_SWB)` returns a non-null encoder
  successfully.
* `evs_enc_process(...)` works on the standalone test (one frame at a
  time, sine-wave input).
* The runner output shows `recreateCodec` reaching
  `creating EVSCodec sr=32000 br=13200` and then hanging in
  `EVSCodec(...)` → `evs_enc_create` (or possibly `evs_dec_create`) on
  the *first* `ensureChannels` call.  No exception, no crash, just a
  silent hang.
* The same configuration worked in the standalone test, so the
  difference is environmental (state already in the ChannelProcessor /
  SignalProcessor before the EVS codec is constructed) – not in the EVS
  API call itself.

Suspected root causes to investigate (in order):

1. **Stack / heap corruption from earlier codecs.**  The runner's main
   loop instantiates the EVS codec *after* processing G.711/GSM/AMR in
   the same process.  Some of those libraries use large static buffers
   or globals that the EVS init may be reading or aliasing.  Try
   building a runner that processes *only* `EVS_NATIVE`.

2. **`init_encoder` for SWB+13.2 k expects extra state.**  Looking at
   `lib_enc/io_enc.c` lines 484–545, the CLI conditionally sets
   `Opt_RF_ON`, `Opt_SC_VBR`, resets `rf_fec_indicator`, and may flip
   `codec_mode` based on `total_brate` and `input_Fs`.  The current
   wrapper only sets `Opt_RF_ON=0` and a hard-coded `MODE1`.  Some
   paths inside `init_encoder` may branch on those flags.  Compare
   `io_enc.c`'s full post-parse state against the wrapper's state and
   pad the missing fields (especially `Opt_DTX_ON`, `Opt_RF_ON`,
   `Opt_SC_VBR`, `var_SID_rate_flag`, `interval_SID`, `rf_fec_indicator`,
   `codec_mode`, `last_codec_mode`).

3. **`codec_mode` mismatch causes an infinite loop in encoder logic.**
   13.2 kbps in MODE1 is the ACELP@13.2 case; in MODE2 it would route
   to a different code path.  Try forcing `MODE2` (the wrapper used
   to do this; switched to `MODE1` for testing – reverting to
   `MODE2` did not help but did not get full coverage).

4. **MSVC /O2 vs /O0 build mismatch.**  The EVS static libs are
   compiled with `/O2` (required by `vo-amrwbenc` and now also applied
   to `evs-lib-*`).  The wrapper sits inside `TelephonyDSP` which links
   those libs but is itself compiled at the global project flags.  A
   mixed-ABI issue between `/O2` (lib) and `/O2` (DSP) is unlikely
   but worth ruling out with `/O0` for the libs as a sanity check.

5. **r8brain resampler state on first call.**  The `CDSPResampler24`
   ctor is invoked with `(48000, 32000, 1024)`.  If the ratio
   calculation misbehaves it could request an out-of-range internal
   buffer, but that would crash rather than hang.  More likely a
   silent failure inside r8brain.  Try with a different input sample
   rate (e.g. force 32 kHz host rate in the runner) to confirm.

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
├── evs_api.c              # float variant (working wrapper, see issue above)
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
VST3 UI for testing. It must not be shipped. `TelephonyRunner` still leaves
`EVS_NATIVE` out of its automatic mode list because of the integration hang
above.

## Open Questions for the Next Agent

1. Why does `init_encoder` for `EVS_NATIVE` (32 kHz / 13.2 kbps / SWB)
   hang in the integration test but not in the standalone test?  Likely
   state-aliasing with the other codec libraries linked into the same
   binary – reproduce with a runner that processes only `EVS_NATIVE`
   to confirm.
2. Finish fixed-point EVS and make `EVS_NATIVE` default to it. This requires
   fixing the current FX header collision (`cnst_fx.h` plus float `cnst.h`) and
   replacing the `evs_api_fx.c` stub with a real wrapper.
3. Should the `EVS_LIKE` mode be promoted to a proper `EVS_DIST`
   filter-only class for the distribution build, or stay as a single
   mode in `SignalProcessor`?
4. Should a dedicated VSTGUI editor be added for a visual route diagram? The
   current implementation exposes `in`, `out`, and `degraded segment` through
   the host's generic VST parameter UI.
5. Are there other 3GPP EVS mirrors worth considering if the lem21h
   one stays unmaintained?  (AOSP branches, etc.)
