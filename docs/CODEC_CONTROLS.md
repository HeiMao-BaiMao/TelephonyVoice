# Codec controls and implementation boundaries

All processing is in-process and uses synthetic test signals. No sockets,
phone calls, microphone capture, or external media services are involved.

## Opus

- Expert packet durations are 2.5, 5, 10, 20, 40, and 60 ms. The public
  `OPUS_SET_EXPERT_FRAME_DURATION` control is applied and PCM framing changes
  with it. Tests inspect the encoded packet's actual sample count.
- SILK / hybrid / CELT readout is derived from the actual encoded packet ToC.
  There is no invented public `OPUS_GET_MODE` control.
- Forced modes use the **bundled private** `OPUS_SET_FORCE_MODE` control from
  pinned Opus commit `f18e26e648e0b7d0ad3db5b98eca461c937ee757`. The real
  private header and compile-time ABI assertions are used. This is not a
  stable public libopus API. SILK/hybrid require at least 10 ms; hybrid also
  requires an input rate of at least 24 kHz, SWB/FB bandwidth and >=16 kbps.
  Codec transition/safety rules can still affect the actual packet mode.
- FEC enable, expected loss percentage, decoder recovery, FEC-only decoding,
  and playout buffering have distinct codec controls. Packets are encoded even
  when transmission is lost, so the following packet can contain real LBRR.
  Recovery counts require `opus_packet_has_lbrr`, not simply a successful PLC
  return. CELT-only packets do not have SILK LBRR.
- The internal transport has a known prebuffer. The host path accounts for
  it separately from the shared packet queue and does not enqueue its startup
  silence as extra uncompensated audio.

## Bit errors and DTX

BER is deterministic for a given seed and applied to encoded payloads before
transport/decode. AMR storage ToCs and padding remain intact; SID mode/update
control bits are protected. GSM's framing magic is preserved. Opus packet
parsing identifies individual coded frames so its ToC, lacing and padding are
untouched. EVS corruption swaps valid G.192 soft-bit symbols while preserving
sync and frame length. Malformed AMR received lengths/types are rejected before
calling the reference decoder.

Native AMR/Opus/EVS DTX uses codec SID/no-data state. Pure-silence comparison
mutes decoded DTX PCM to exact zero while continuing the native decoder's CNG
state. G.711/GSM use the supplied VAD decision and simulated comfort noise;
they do not claim a native SID bitstream extension. PSD noise is the default
legacy concealment. Its explicit disabled setting uses attenuated last-frame
repetition for comparison.

The AMR-WB encoder's vendored `Copy` routine overread its source by two elements.
`helpers/amrwb_util_safe.c` replaces that source at build time with bounded
loops, retaining its license and leaving the pinned submodule unchanged.

## EVS runtime profiles

AMR-NB/WB mode changes preserve encoder and decoder histories. EVS changes use
the reference bitrate/bandwidth profile fields and preserve transition state;
invalid configurations fail without applying the new configuration. Unchanged
UI configurations are no-ops, including adaptive SID state. This is deliberately
not equivalent to repeatedly issuing a reference CLI profile command, which
resets some per-frame profile fields even when its value is unchanged.

Auto bandwidth is a heuristic 256-point Hann-windowed spectral estimate, with
energy thresholds, a two-frame upward/eight-frame downward hold, and bitrate /
sample-rate validity limits. It is not a standardized bandwidth classifier.
It changes the live bandwidth ceiling without destroying codec state.

AMR-WB interoperability is genuine reference `amr_wb_enc` / `amr_wb_dec`
processing at all nine AMR-WB rates. The wrapper retains the original native
rate, bandwidth ceiling, and SC-VBR selection across IO entry/exit. Live IO
rate changes and explicit reset are supported. SID STI/CMI metadata is captured
from the encoder's actual state before its G.192 indices are cleared.
SC-VBR also retains the selected fixed-rate profile for restoration when it is
disabled. The floating-point and official fixed-point adapters share these
configuration APIs. The experimental JBM route currently excludes AMR-WB IO.

## Real EVS JBM clock mismatch

The JBM route has separate source and receiver clocks. A bounded streaming
source-sample accumulator produces zero, one, or two complete encoder frames
per receiver 20 ms deadline. Positive ppm means a faster receiver and fewer
source packets; negative ppm produces more source packets. Source RTP sequence
and timestamps advance per encoded packet, while network transmission and
arrival are scheduled against receiver deadlines.

The batch transport drains every actual arrival, including reordering and
duplicates, into the reference JBM; it does not place another fixed playout
queue in front of the JBM. The unmodified `EvsRXlib` invokes the real APA
scaler to handle its buffer occupancy. The phase accumulator supplies source
rate mismatch; it does not replace APA with a claimed equivalent algorithm.
Other codec routes use the separate elastic clock-drift simulation.

The JBM safety margin is configurable (default 60 ms). Explicit zero is passed
through by the new API, while the legacy C constructor's zero retains its
historical default. The reference still enforces a minimum startup target of
60 ms and can adapt its wet-path delay thereafter. This adaptive delay is a
simulated effect, not an exact fixed VST compensation amount.

## Synthetic regression coverage

`TelephonyCodecControls` covers all Opus packet durations, actual forced modes,
LBRR recovery, deterministic BER, container protection, native/legacy pure DTX,
AMR state-preserving mode switches, all nine EVS IO rates and SID metadata,
live IO rate changes, native-profile restoration, auto bandwidth, dense EVS
bit-error decoding, JBM packet hooks, and SC-VBR restoration.

`TelephonyJbmClockDrift` separately checks source-sample and packet counts at
+/-50 ppm over a long synthetic run. On GNU-linker platforms it wraps calls to
the actual reference `apa_set_scale` and `apa_exec`, observing non-unity scale
requests and actual inserted/removed samples without modifying reference code.
