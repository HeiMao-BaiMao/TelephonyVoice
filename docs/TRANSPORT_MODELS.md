# Internal transport models and protocol scope

This document describes `dsp/NetworkProfile.*`, `dsp/Transport.*`,
`dsp/TransportEvs.*`, and the encoded-frame adapter in
`dsp/ChannelTransport.*`. These are in-process simulation components. They do
not open sockets or implement signaling, session negotiation, encryption, or a
network endpoint.

## Implementation map

| Roadmap area | Implemented behavior | Scope boundary |
|---|---|---|
| Independent network controls | Separate loss, burst, jitter, shape, correlation, duplicate, late-arrival and playout settings | Bandwidth filtering and encoder FEC controls are applied by the channel/codec, not by the byte queue |
| Gilbert–Elliott | Good/Bad transitions and separate state-dependent loss emissions; derived loss/burst configuration | Not a substitute for standardized recorded radio-error vectors |
| Jitter | Uniform, Gamma, Weibull and Pareto innovations with optional AR(1) correlation | Finite physical delay clamp changes the resulting distribution near the limits |
| Reordering/late/duplicate | Arrival-ordered network queue followed by extended-sequence-ordered playout; deadline and duplicate rejection | Bounded, owning containers; not a lock-free or allocation-free transport implementation |
| RTP | Fixed header, CSRCs, extensions and padding; real serialized packets through the adapter queue | Extension bodies are opaque; no SDP/RTP session machinery |
| AMR | RFC 4867 mono octet-aligned and bandwidth-efficient packing; storage adapters | Optional CRC, interleaving and robust sorting are not implemented |
| EVS | Compact/Header-Full payloads, Primary and AMR-WB IO bit formats, G.192 conversion | Payload support alone does not establish support in a particular codec build |
| RED | RFC 2198 block serializer/parser; one previous encoded frame protected by the application adapter | Recovery needs the successor before the missing frame's playout deadline |
| Opus FEC | Successor packet exposed without removing it; codec uses `opus_decode(..., decode_fec=1)` | There is no FEC flag in the Opus ToC byte |
| RTCP | Measured reception statistics; SR/RR and XR RRTR generation; accessible encoded snapshots | No report transmission, SDES session policy, or simulated control-path RTT |

The generic packet libraries support some combinations beyond those selected by
the plugin adapter. The adapter emits one codec frame per RTP packet and handles
one audio channel per `ChannelTransport` instance. It keeps the wire options
with each queued frame, so changing AMR alignment or enabling/disabling RTP does
not reinterpret already queued bytes using the new format.

## Loss model and reproducibility

`NetworkProfile` is sanitized before use: probabilities, queue size, delay
limits, distribution shape and correlation are bounded; nonfinite controls get
finite defaults. Each simulator instance owns its random state. Loss, jitter,
duplication and late-arrival decisions use separate seeded streams, so changing
a jitter distribution does not alter the seeded packet-loss realization.
Reset repeats the realization. There is no process-wide random generator or
wall-clock seed. Floating-point distribution results still depend on the
platform's elementary math functions; cross-platform bit-exact PCM is not
promised.

The Gilbert–Elliott parameters are:

- `p`: Good to Bad transition probability
- `r`: Bad to Good transition probability
- `k`: successful reception probability while Good
- `h`: successful reception probability while Bad

The current state produces a loss decision before the next-state transition.
The initial state after reset is Good. For `p + r > 0`, the stationary loss
probability is `(r * (1-k) + p * (1-h)) / (p+r)`.

The derived loss/burst helper uses all-good/all-bad emissions. It preserves the
requested stationary loss and increases the effective burst duration when the
requested combination would require an impossible transition probability above
one. The explicit Good/Bad controls take precedence over the base independent
packet-loss rate. The additional loss boost remains a separate Bernoulli loss
process; for independent base loss `L` and boost `B`, total loss is
`1 - (1-L)*(1-B)`, not `L+B`.

The adapter selects independent loss when mean burst is one and explicit
Gilbert controls are disabled. A larger mean burst selects the derived
Gilbert model. An explicitly lost codec frame is still packetized before
being dropped, allowing the next RED packet to protect it and keeping sender
counters distinct from receiver counters.

## Jitter and arrival semantics

Let `A` be jitter amplitude and `s` be shape. Before correlation, the normalized
innovations are:

- Uniform: `2*U - 1`
- Gamma: `Gamma(s, 1)/s - 1`
- Weibull: `(-log(U))^(1/s) / Gamma(1+1/s) - 1`
- Pareto: `U^(-1/s)*(s-1)/s - 1`

`U` is strictly between zero and one. Thus the positive distributions are
mean-centered, and `A` is a scale control, not a universal standard deviation.
Pareto shape is clamped above two to retain finite variance. The AR(1) filter
uses `j[n] = rho*j[n-1] + sqrt(1-rho*rho)*innovation[n]`. When correlation is
enabled, the marginal distribution is the filtered distribution rather than
the unfiltered named distribution.

The low-level simulator permits a separate nominal delay. The application
adapter sets nominal delay equal to jitter amplitude, then adds the centered
jitter and any explicit late-arrival delay. Arrival cannot precede the send
time. The adapter caps total network delay at 500 ms. Consequently, an early
arrival means early relative to the nominal path delay; it does not mean time
travel. Clipping a heavy tail or negative offset changes its observed moments.

Network arrivals are ordered by arrival time, with an extended sequence number
as a stable tie-breaker. After arrival, packets enter the sequence-indexed
playout buffer. Playout asks for a specific sequence at a deadline, rather than
decoding whichever packet arrived first. Duplicates do not displace the
original. Frames past their deadline are not replayed later. `peek()` exposes a
future packet for FEC or RED without consuming its normal playout slot.

The default network and playout capacities are 512 packets. Capacity rejection
is counted separately from model loss. The adapter retains a bounded metadata
history for late frames. Containers own encoded byte vectors and can allocate;
this implementation must not be described as allocation-free real-time DSP.

## Clock, sequence and playout

Internal sequence numbers are extended 64-bit counters. The RTP wire sequence
is their low 16 bits. Packet ordering never depends on an ambiguous ordinary
comparison of wrapping 16-bit values.

The frame clock advances by `frameSamples / sampleRate`. With host-clock mode,
the session is anchored to the host timeline and timeline discontinuities reset
transport state. Within host processing blocks it remains a sample-derived
virtual clock. It is not a live socket clock, and musical beat positions must
be converted by the host adapter before reaching `setTime(seconds, playing)`.
Negative host pre-roll maps safely to wrapping RTP timestamps.

RTP timestamp clocks are independent of PCM rate where required:

- Opus: always 48 kHz, per [RFC 7587 section 4.1](https://www.rfc-editor.org/rfc/rfc7587.html#section-4.1)
- EVS Primary and AMR-WB IO: 16 kHz, per [TS 26.445 Annex A.3.2](https://www.etsi.org/deliver/etsi_ts/126400_126499/126445/19.01.00_60/ts_126445v190100p.pdf)
- AMR-NB/GSM/G.711: 8 kHz; AMR-WB: 16 kHz

Initial playout prebuffering is rounded up to an integer number of codec
frames. `warmingUp()` distinguishes that prebuffer from missing media. The
audio channel and host latency contract are responsible for accounting for
that delay once. Changing codec frame duration or playout delay requires the
channel-level framing reset; the low-level queue does not reinterpret the
duration of packets already queued.

### Asynchronous EVS reference-JBM ingress

`EVSCodecJbm` uses `ICodecTransport::exchangeBatch` instead of the synchronous
one-packet `exchange` path. Its bounded fractional sample accumulator produces
zero, one or two encoded frames per nominal 20 ms receiver callback. Every
encoded frame has its own extended source sequence and a media timestamp that
advances by 20 ms. The receiver callback deadline and physical send time advance
on the separate nominal clock. In particular, the network queue must not use
the drifting media timestamp as the physical send time, because doing that
would cancel the intended source/receiver clock difference.

Batch ingress performs the same real RTP, AMR, EVS and optional RED serialization
as synchronous ingress. It submits all encoded frames to `NetworkSimulator`,
then drains every packet whose actual arrival time is at or before the current
receiver deadline. Empty sender batches still drain existing arrivals. Returned
frames preserve original source sequence, media timestamp, physical send time,
arrival time and the receiver callback deadline. Duplicate and reordered
arrivals are passed to the reference JBM with their original identity. There
is no second `PacketJitterBuffer`, per-tick missing-frame declaration, or fixed
transport prebuffer in this path. Profile playback delay configures the real
EVS reference receiver's safety margin; adaptive JBM delay is wet-path behavior
rather than another fixed host-latency compensation queue.

If a received RED successor contains a missing previous frame, the adapter
reconstructs that frame at the successor's actual arrival time. The reference
JBM decides whether it is still usable. This reconstruction does not invent an
RTP reception or remove the original network loss from RTCP/transport counters.
Queued payloads retain their original format settings through live changes.

Batch telemetry's packet count is the number of encoded submission attempts;
loss is known network drops, rejected originals at queue capacity, and adapter
validation/serialization failures divided by those attempts. In-flight packets
are not losses. `pendingPackets()` exposes the current network queue size,
including queued duplicates. The synchronous path instead measures missing
frames at its fixed playout deadline. Batch telemetry deliberately does not
label an arrival as late relative to an invented transport deadline: actual
late-frame acceptance and concealment belong to the reference JBM. RTCP reports
continue to observe only actual RTP arrivals, including duplicates according
to the RFC reception-counter rules, and sender packet/octet counts reflect
serialized RTP traffic even when the network drops it.

## Packet format details

### RTP and AMR

The RTP layer checks version, fixed-header length, CSRC count, extension length
and padding count. It handles network byte order explicitly. It preserves an
opaque aligned extension body. See [RFC 3550 section 5.1](https://www.rfc-editor.org/rfc/rfc3550.html#section-5.1).

The AMR layer uses the speech-bit counts and sensitivity ordering expected by
the existing opencore AMR-NB and AMR-WB storage APIs. It supports all eight
AMR-NB speech modes, all nine AMR-WB speech modes, SID and NO_DATA, plus AMR-WB
SPEECH_LOST. Reserved frame types and length mismatches are rejected. Reserved
incoming CMR values mean no actionable request. Receivers ignore reserved and
alignment bits; serializers emit canonical zero padding. See
[RFC 4867 sections 4.3–4.4 and 5](https://www.rfc-editor.org/rfc/rfc4867.html).

The low-level AMR payload can hold multiple mono frames, but the adapter emits
one. Negotiated CRC, interleaving, robust sorting and multichannel layouts are
not supported or auto-detected. The storage adapters handle one ToC octet plus
one frame, not a complete file with its magic header.

### EVS

`TransportEvs` follows [TS 26.445 Annex A.2](https://www.etsi.org/deliver/etsi_ts/126400_126499/126445/19.01.00_60/ts_126445v190100p.pdf)
and uses [TS 26.201 IF1 ordering](https://www.etsi.org/deliver/etsi_ts/126200_126299/126201/19.00.00_60/ts_126201v190000p.pdf)
for AMR-WB IO. It handles every defined Primary/IO frame length, CMR and ToC
chains, compact IO bit rotation, the seven-byte SID discriminator and
Header-Full collision-avoidance padding. Format detection is based on the
negotiated mode and protected payload lengths; it does not try Header-Full and
guess another format after failure.

`headerFullOnly` represents the local equivalent of negotiated `hf-only=1`.
The adapter's Auto and Compact-preferred settings both use compact when it can
represent the current frame, allowing Header-Full when SID/NO_DATA requires it.
The Header-Full setting uses `headerFullOnly` consistently on both ends. At the
generic library level, explicitly requesting Compact for an unrepresentable
frame returns an error instead of silently changing the request.

G.192 conversion accepts explicit native/little/big-endian 16-bit words and
validates the sync, bit count and bit symbols without aligned pointer casts.
Primary frames retain natural bit order. IO conversion includes the IF1
permutations. IO SID metadata carries STI and CMI separately because bare
G.192 does not contain all of that information; zero-bit IO frames cannot be
unambiguously treated as SID_FIRST without side information. The codec adapter
provides that metadata when available.

Generic Header-Full payload grouping supports multiple channels and frames;
the plugin adapter uses one channel and one frame. CMR syntax support does not
imply negotiated rate adaptation or automatic application of incoming CMRs to
the encoder. No SDP is sent or received.

### RED and Opus FEC

The generic [RFC 2198](https://www.rfc-editor.org/rfc/rfc2198.html) layer supports
multiple redundant blocks with 14-bit timestamp offsets and 10-bit block
lengths. The adapter protects at most the immediately preceding encoded frame,
only when its payload type matches and it fits those bounds. The receiver
matches redundant data against the missing frame's stored timestamp, including
timestamp wrap, rather than inferring it from current playout settings.

The successor must already have arrived by the current frame's deadline. No
extra unreported delay is introduced to wait for redundancy. A burst longer
than the retained redundancy depth cannot be reconstructed. RED recovery does
not remove the successor's primary packet from the queue.

Opus redundancy is separate from RFC 2198. The successor is passed to the
codec as `nextPayload`; the codec performs in-band FEC decoding and retains it
for subsequent normal decoding. FEC availability is not encoded in a standalone
Opus ToC flag. `decode_fec=1` may produce concealment when useful in-band
redundancy is absent. The codec's LBRR inspection distinguishes actual FEC
recovery from that fallback. See [RFC 6716](https://www.rfc-editor.org/rfc/rfc6716.html)
and [RFC 7587](https://www.rfc-editor.org/rfc/rfc7587.html).

## RTCP statistics and readouts

`ReceiverStatistics` implements RFC 3550 sequence validation, interval loss and
interarrival jitter. The internal source is already known, so its initial
two-packet source-discovery probation is omitted. Large sequence jumps still
need two consecutive packets to confirm a restart. Duplicate receptions count
as received, so cumulative RTCP loss can be negative; interval fraction loss
never becomes negative. RTP loss and playout loss are different measurements.

The adapter generates SR, RR and an XR Receiver Reference Time block about
once per simulated second after initial prebuffering. Sender counts include
serialized packets later lost in the network. Octet counts include the actual
RTP payload, including RED framing when used, and exclude the RTP header.
NTP fields represent elapsed session time. Prior SR timestamps populate LSR
and DLSR through an explicit zero-delay in-process control path.

Snapshots are available through `senderReportBytes()`, `receiverReportBytes()`,
`extendedReportBytes()` and `receiverReport()`. `networkCounters()` exposes
model drops, injected duplicates, deliveries, reordering and capacity events.
The shared processing telemetry reports measured playout loss, jitter in ms,
and late/duplicate counts. Individual RTCP fraction/cumulative-loss fields are
available in the transport API; they are not separate GUI/CLI controls.

The generic RTCP library serializes/parses SR, RR and XR, including multiple
supported packets in a datagram. XR provides RRTR and DLRR builders and retains
opaque block bodies with structural checks. The adapter currently generates
RRTR; it does not claim automatic generation of all seven RFC 3611 block
families, a full VoIP metrics estimator, or an RTT measurement. Full compound
RTCP session behavior with SDES/BYE and RTCP scheduling bandwidth rules is not
implemented. See [RFC 3550 section 6 and Appendix A](https://www.rfc-editor.org/rfc/rfc3550.html)
and [RFC 3611](https://www.rfc-editor.org/rfc/rfc3611.html).

## Reference-pattern correction and remaining data gap

The roadmap's phrase “TS 26.131 EP1–EP6 presets” combines different things.
EP1–EP6 are specified in [TR 45.050 Annex F.2](https://www.etsi.org/deliver/etsi_tr/145000_145099/145050/19.00.00_60/tr_145050v190000p.pdf)
as GSM half-rate development test data. Each pattern has a soft-decision/chip
error file and a corresponding TCH/FS error file:

| Pattern | Soft decisions/chip errors | TCH/FS errors |
|---|---|---|
| EP1 | `SDCEPCI10RFFH_1.DAT` | `EPTCHFSCI10RFFH_1.DAT` |
| EP2 | `SDCEPCI7RFFH_1.DAT` | `EPTCHFSCI7RFFH_1.DAT` |
| EP3 | `SDCEPCI4RFFH_1.DAT` | `EPTCHFSCI4RFFH_1.DAT` |
| EP4 | `SDCEPCI10RFNFH_1.DAT` | `EPTCHFSCI10RFNFH_1.DAT` |
| EP5 | `SDCEPCI7RFNFH_1.DAT` | `EPTCHFSCI7RFNFH_1.DAT` |
| EP6 | `SDCEPRAN_1.DAT` | `EPTCHFSRAN_1.DAT` |

These are not four Gilbert–Elliott parameters or a list of packet erasures.
They require the real files and a defined radio/channel-decoding interpretation.
The files are not included in this checkout, and there is no compatible binary
loader or channel-decoding adapter here. No invented probabilistic presets are
labeled EP1–EP6.

[TS 26.131](https://www.etsi.org/deliver/etsi_ts/126100_126199/126131/18.01.00_60/ts_126131v180100p.pdf)
instead references IP-layer delay/loss profiles such as
`dly_profile_20msDRX_10pct_BLER_e2e`,
`dly_profile_40msDRX_10pct_BLER_e2e`, and
`dly_profile_40msDRX_22pct_BLER_e2e`. Its table notes identify their electronic
attachments as part of TS 26.132. Those actual profile files and their importer
are also absent. Therefore standardized trace presets remain a data/import
gap, not a completed feature.

There is low-level custom trace support: `NetworkSimulator::setErrorPattern`
accepts a nonempty string containing `0`, `1` and whitespace, where `1` means a
dropped packet. The caller selects `NetworkLossModel::ErrorPattern`. Repeating
traces wrap; a nonrepeating exhausted trace produces no further base trace
losses. The independent loss boost can still apply. Invalid input leaves the
previous trace unchanged. This API is not exposed as a GUI/CLI file-import
feature, and it does not parse either standardized binary/profile format.

## Verification

The following CTest targets are registered:

| Target | Coverage |
|---|---|
| `transport_protocols` | 55 assertion groups, known RTP/AMR/RED/RTCP vectors, malformed/truncated inputs, sequence/timestamp wrap, loss and burst statistics, jitter means/correlation, reordering/late/duplicate behavior, 20,000 deterministic malformed payloads |
| `transport_evs` | 11,669 checks covering all defined Primary/IO modes, compact/Header-Full, malformed input, padding/discriminators, channel grouping and G.192/IF1 conversion |
| `transport_integration` | 25 assertion groups covering encoded adapter queues, RED recovery, retained FEC successor, deadlines, live format changes, report counts, clock rates and negative host pre-roll |
| `transport_batch_ingress` | Asynchronous 0/1/2-frame ingress, independent source/physical clocks, source sequence wrap, empty-batch draining, pending/loss counters, duplicates/reordering, RED recovery, live payload metadata, EVS framing and bounded queues |
| `transport_amr_codecs` | Non-distribution only: 3,400 real AMR-NB/WB frames over all 17 speech modes, including 357 SID and 2,379 NO_DATA; both payload alignments preserve bytes and decoded PCM |

The isolated protocol and adapter tests pass with strict compiler warnings and
AddressSanitizer/UndefinedBehaviorSanitizer. LeakSanitizer was unavailable under
the development environment's ptrace wrapper; those sanitizer runs used
`ASAN_OPTIONS=detect_leaks=0` and are not leak-check evidence.

An additional development smoke test round-tripped 1,080 frames from the real
floating-point EVS encoder through G.192 and EVS payloads, then decoded them.
It included 48 SID, 456 NO_DATA and 11 SC-VBR 2.8 kbps frames. This was a
separate smoke test, not an additional registered CTest target.

Run `ctest --test-dir <configured-build-directory> -R transport --output-on-failure`
after building the test targets. Passing these focused checks is not a claim
that every host, UI state, codec variant, latency path or full application
build configuration has passed its separate aggregate validation.
