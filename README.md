# TelephonyVoice

VST3 plugin (and CLI runner) that emulates various telephone / cellular voice
codecs on the audio.  Personal-use builds can swap the codec stubs for the
real 3GPP reference implementations.

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

It produces `<input>.<mode>.wav` for each era (G.711, GSM FR, AMR-NB, AMR-WB,
EVS-like) in parallel.

## Modes

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
  build option).  Not implemented.
* `TelephonyDSP` extended with an `EVS_NATIVE` mode, `EVSCodec` class
  wrapping the C API, EVS configuration plumbing on `ChannelProcessor` and
  `SignalProcessor`.  Default config: SWB 32 kHz / 13.2 kbps.
* `TelephonyRunner` was wired up to call `setEVSConfig` for `EVS_NATIVE`.
  After discovering the hang (below) the iteration was commented out so the
  CLI ships in a usable state.

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

This builds the fixed-point variant (TS 26.442 v16.4.0).  The float
wrapper (`evs_api.c`) is replaced by the stub `evs_api_fx.c`, which
currently just returns errors.  A full implementation of `evs_api_fx.c`
would mirror the float version but use `Encoder_State_fx` /
`init_encoder_fx` / `evs_enc_fx` / `Decoder_State_fx` /
`init_decoder_fx` / `evs_dec_fx` and the Q-format Word16 buffers.

## Distribution / Non-EVS Edition (Not Started)

User requirement: a separate, distributable build that drops
AMR/AMR-WB/EVS entirely (the codecs that have patent / 3GPP-member
encumbrance) and only ships G.711, GSM and the EVS_LIKE filter stand-in.

The cleanest path:

* Add a CMake option `TELEPHONY_DISTRIBUTION_BUILD=ON` that:
  * Excludes `cmake/vo-amrwbenc.cmake`, `cmake/opencore-amr.cmake`,
    `cmake/3gpp-evs.cmake`.
  * Adds a tiny `codec_emu` library that re-implements AMR-NB /
    AMR-WB / EVS_LIKE / EVS_NATIVE "LIKE" modes with cascading biquads
    plus band-limited noise (the EVS_LIKE branch already does this;
    just promote it to a proper class).
* Make `EraMode::EVS_NATIVE` and `EraMode::AMR_*` compile-out under
  that option (or alias them to `EVS_LIKE` / `Bypass`).

This is sketched out but not implemented.

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
├── TelephonyDSP.h         # public C++ API of the SignalProcessor
├── TelephonyDSP.cpp       # codec emulations, resampler/filter chain
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
| (planned) `TELEPHONY_DISTRIBUTION_BUILD` | OFF | Strip AMR/AMR-WB/EVS, build LIKE versions only.                |

## License Note

For personal use, all libraries are permissively licensed
(Apache 2.0 / MIT / public domain / custom permissive).  However,
**AMR/AMR-WB/EVS codec patents** (VoiceAge, Fraunhofer, NTT DoCoMo,
Ericsson, Nokia, etc.) apply to commercial distribution.  The
distribution-only build (planned, not implemented) addresses this.

The current `evs_native` mode must not be shipped.  It is gated by
`EraMode::EVS_NATIVE`, which is left disabled in `TelephonyRunner`
(commented out) and not exposed in the VST3 UI.

## Open Questions for the Next Agent

1. Why does `init_encoder` for `EVS_NATIVE` (32 kHz / 13.2 kbps / SWB)
   hang in the integration test but not in the standalone test?  Likely
   state-aliasing with the other codec libraries linked into the same
   binary – reproduce with a runner that processes only `EVS_NATIVE`
   to confirm.
2. Is the fixed-point variant worth wiring up?  Pros: real production
   implementation, deterministic, faster.  Cons: extra integration
   effort, more Q-format conversion.
3. Should the `EVS_LIKE` mode be promoted to a proper `EVS_DIST`
   filter-only class for the distribution build, or stay as a single
   mode in `SignalProcessor`?
4. Are there other 3GPP EVS mirrors worth considering if the lem21h
   one stays unmaintained?  (AOSP branches, etc.)
