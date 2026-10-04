# Internal audio simulation models

These effects operate on synthetic or DAW-supplied PCM entirely inside the
plugin. They open no audio devices or network sockets. They are engineering
models for reproducible audible experiments, not a certification implementation
of a mobile radio, PSTN line, or complete 3GPP network.

Implementation: `dsp/AudioSimulation.h/.cpp`; tests:
`tests/AudioSimulationTests.cpp` (`audio_simulation` in CTest). The DSP classes
support 8,000–384,000 Hz, validate finite parameters, sanitize non-finite audio,
retain stream state across arbitrary host block boundaries, and restart
repeatably on reset. The plugin runs these classes at the selected codec's
internal rate after host-rate conversion. Configure/reset is a control operation;
the streaming processing methods allocate no memory.

## PSD-shaped comfort noise (roadmap 2.1)

`PsdComfortNoise` uses 256-sample Hann-windowed periodograms with a 128-sample
hop. The first estimate becomes available after 128 good samples; unobserved
history is zero-padded. Power spectra and good-frame RMS are exponentially
smoothed with a 150 ms time constant. The square root of the spectrum is
inverse-transformed, centered, and Blackman-windowed into a 129-tap FIR.

A seeded xorshift innovation stream with unit variance passes through that FIR.
Unit-energy FIR normalization and the measured RMS provide gain matching.
Output is bounded to full-scale PCM. Clipping can reduce RMS matching for
near-full-scale inputs. Only good samples train the estimator: generated
concealment never feeds its own spectral estimate.

This is shaped comfort noise, not pitch reconstruction, a codec-native packet
loss concealment algorithm, or a standardized SID bitstream. Spectral resolution
is limited by the 256-sample transform, particularly at high sample rates.
`WaveformConcealer` retains its compatibility name but now delegates to this
model instead of repeating previous speech waveforms. The optional channel
PSD control also permits explicit comfort-noise substitution for simulated
legacy-codec DTX/loss.

## DTMF test source (roadmap 2.2)

The generator implements all 16 keypad frequency pairs: low group 697, 770,
852, 941 Hz and high group 1209, 1336, 1477, 1633 Hz, as specified by
[ITU-T Q.23](https://www.itu.int/rec/T-REC-Q.23/en). The
[ITU-T G.720 DTMF description](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-G.720-199507-I%21%21PDF-E&lang=f&type=items)
also lists these groups. Equal-amplitude tones use a 2 ms raised-cosine edge
ramp. The plugin offers 50–100 ms tone duration and at least 50 ms gaps. The
standalone sequence API also permits longer test tones. Invalid characters are
ignored; `a`–`d` are accepted as `A`–`D`.

The level represents the peak bound of the summed pair, not each individual
sinusoid's RMS. Each sinusoid has half that peak amplitude. A zero linear level
is exact silence. The source is inserted before the codec, so codec distortion
of DTMF is audible. It does not emit RFC 4733 telephone-event packets or claim
Q.24 receiver conformance.

## Sample-clock drift (roadmap 3.4)

`ClockDriftResampler` reads a bounded elastic FIFO at
`1 + ppm / 1,000,000` input samples per output sample. Positive ppm consumes
samples faster; negative ppm consumes them more slowly. Linear fractional
interpolation realizes the small rate offset. The default FIFO begins with
2 ms of silence. The plugin incorporates this reserve into its latency handling.

At a FIFO boundary a one-sample repeat or skip recovers occupancy and increments
an underrun/overrun counter. This produces actual accumulated clock mismatch,
rather than just changing an unrelated delay parameter. It is a linear
resampler with sample-slip recovery, **not the EVS reference APA time scaler**.
Interpolation has its expected high-frequency response; no pitch-preserving
WSOLA/APA or standards jitter-buffer controller is asserted.

## Filtered hybrid/line echo (roadmap 3.5)

The feedback path has two cascaded unity-DC-gain one-pole low-pass filters,
a one-pole high-pass filter, a sample delay, and return gain. The high-pass
impulse response has absolute sum at most 2 and the low-pass sums are 1;
limiting return gain to 0.45 keeps the loop's absolute gain below 1. This proves
bounded-input/bounded-output stability even before the full-scale safety limit.
The maximum gain is enforced in DSP regardless of the caller's input.

`inject(dry)` and `capture(processedOutput)` can feed actual codec output back
into its input. `process(dry)` is a standalone self-feedback convenience.
In the plugin's frame-based codec integration the feedback is taken from the
previous decoded codec frame. Consequently the physical modeled return delay
is **the selected echo delay plus one codec frame**, in addition to filtering
phase delay; it is not simply the echo-delay knob. The original codec output
continues feeding the echo state during a handover mute.

This model is not a measured handset/hybrid impulse response or an echo
canceller, and does not claim a G.168 test case.

## Flat fading and C/I adaptation (roadmap 4.1)

`RayleighFading` uses 32 sinusoidal Doppler components in each quadrature,
seeded independent phases, and midpoint angular quadrature of isotropic arrival
angles. It is a finite sum-of-sinusoids approximation to the Clarke/Jakes
flat-fading statistics, motivated by the scattering model in
[R. H. Clarke, “A Statistical Theory of Mobile-Radio Reception,” 1968](https://onlinelibrary.wiley.com/doi/10.1002/j.1538-7305.1968.tb00069.x).
Oscillators advance sample by sample with periodic normalization, so block
partitioning does not alter the trajectory. Ensemble mean power is normalized
to one; a finite trajectory is not guaranteed to have exact unit mean power.

The exposed power/envelope retains the fading statistics. Audio application
uses `min(envelope, 1)`, deliberately attenuating without boosting peaks. A
configured mean C/I plus the instantaneous fading-power term produces the
mode-selection input, smoothed over 100 ms. Mapping −5 dB to the lowest mode
and 25 dB to the highest mode is a **local engineering heuristic**, not a
3GPP-mandated AMR/EVS adaptation rule. There is no RF modulation, path-delay
profile, interference waveform, antenna/MIMO model, or TS 45.008 / TS 36.101
conformance claim.

## Handover interruption (roadmap 4.2)

`HandoverGate` mutes for exactly the configured 50–200 ms gap and fades back
in with a 5 ms raised-cosine recovery by default. An explicit trigger or a
periodic interval may start it. The first periodic event occurs after one
interval. A configured interval shorter than gap plus recovery is lengthened
to keep events from overlapping. Zero interval disables periodic triggering.

The codec continues processing through the interruption, so its internal state
is retained and decoded audio is ready when the mute ends. This is a local
audible outage experiment, not inter-cell signaling or a simulated base-station
handover protocol.

## Reference-core VAD2 and hangover (roadmap 4.5)

Non-distribution builds compile the bundled OpenCORE reference-derived VAD2
algorithm, including its spectral channels, adaptive noise estimate, primary
voice metric, burst count, and SNR-dependent hangover. It is not a simple RMS
threshold relabeled as VAD2. The VAD2 core is defined in
[3GPP TS 26.094, section 4, published as ETSI TS 126 094](https://www.etsi.org/deliver/etsi_ts/126000_126099/126094/14.00.00_60/ts_126094v140000p.pdf).
That specification processes two 10 ms subframes per 20 ms AMR frame and
expects encoder high-pass output plus a long-term-prediction flag.

Two entry paths are intentionally distinguished:

- `processReferenceFrame`: exactly 80 already-preprocessed 8 kHz PCM16 samples
  and the caller's encoder-derived LTP flag. It invokes the actual reference
  core and exposes its primary/hangover/burst state.
- `process` / `processPCM16`: a standalone front-end, using a four-pole
  low-pass before downsampling to 8 kHz, an 80 Hz high-pass, and normalized
  autocorrelation for an estimated LTP flag. This convenient standalone path
  is **not bit-exact to the complete AMR encoder's mandatory front-end**.
  Its downsampler is a bounded low-cost approximation, not a reference
  resampling filter or a full-band speech classifier.

`AudioSimulationVad2Core.cpp` adapts historical operation names;
`AudioSimulationVad2Fft.cpp` preserves the bundled reference-derived FFT and
its license while replacing an absent header with a safe equivalent deposit
operation. Arithmetic entry points use the existing OpenCORE basic operations
and isolated ABI. Third-party submodules are not modified.

Without that backend, `available()` is false and the class does not invent a
fallback VAD result. Distribution builds must present VAD2 as unavailable;
the separate energy/hangover mode is not a standards VAD2 implementation.
No formal 3GPP conformance/bit-exact vector certification is claimed.

## Reproducible validation

`audio_simulation` covers:

- DTMF spectral peaks, level, silence gaps, keypad coverage, partition/reset
- PSD color, shaped-output spectral balance, RMS gain, startup silence,
  compatibility PLC, partition/reset
- Positive/negative 50 ppm FIFO drift, real boundary recovery, finite bounds,
  zero-ppm pure delay, partition/reset
- Echo onset delay, tail decay, feedback bounds, partition/reset
- Fading ensemble power/CDF, deterministic seed/reset, C/I mode extremes
- Exact handover mute/recovery duration and periodic count, partition/reset
- Actual VAD2 speech decisions, hangover, return to silence, resampled frame
  counts, irregular-block equivalence, reset, and backend-unavailable behavior
- NaN/invalid-parameter containment

The tests use synthetic generated signals only. ASan/UBSan standalone runs
cover both generic modules and the VAD2 adapters. In the current traced Linux
executor LeakSanitizer cannot run; address/undefined-behavior checks run with
`ASAN_OPTIONS=detect_leaks=0`. Test success establishes these synthetic
invariants, not subjective speech quality or telecommunications conformance.
