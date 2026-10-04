# EVS fixed-point reference integration

## Source requirement

`TELEPHONY_USE_EVS_FX=ON` requires a complete, separately supplied official
3GPP TS 26.442 v16.4.0 `c-code` directory. Set
`TELEPHONY_EVS_FX_SOURCE_DIR` to that directory. The repository does not download,
redistribute, or modify this reference package.

The pinned `external/3gpp-evs` commit is a mixed TS 26.443 floating-point tree
with incomplete fixed-point additions. Its shared state structures are
floating-point, and several required fixed-point algorithms are absent. It is
not accepted as the FX source. The historical parent `*_fx.c` link helpers, `helpers/acelp_float_wrap.c` and
its private `acelp_43bit_float.c` include have been removed; Git history retains
them.
A missing/incorrect source directory causes a configure error, rather than
producing a stub or silently selecting a floating-point codec.

Official source and specification:

- [ETSI TS 126 442 v16.4.0 source directory](https://www.etsi.org/deliver/etsi_ts/126400_126499/126442/16.04.00_60/)
- Source attachment: `ts_126442v160400p0.zip`
- SHA-256 of attachment used for local evaluation:
  `21f43028ff09753ceec3a827886b575b3dcacb3f2997ae5e8dedaf166b1d98b1`
- Nested `26442-g40-ANSI-C_source_code.zip` SHA-256:
  `c2621dbd02969e6d2cbe20c8c34068d83604f5bc7bccf691110b58d198987e9f`
- [Normative specification](https://www.etsi.org/deliver/etsi_ts/126400_126499/126442/16.04.00_60/ts_126442v160400p.pdf)

Extract both ZIP levels. The readme identifies the November 4, 2021 reference
shared by TS 26.442 versions 12.15.0, 13.10.0, 14.6.0, 15.4.0 and 16.4.0.
Only source is compiled; supplied executables are not used.

## Usage conditions

This is a standards reference implementation, not a newly granted open-source
or patent license. The package's `basic_op/basop.rme` and `basop32.c` refer to
the ITU-T General Public License associated with G.191. The ETSI specification
includes copyright, warranty and essential-IPR notices.

ETSI's [IPR explanation](https://www.etsi.org/resources/intellectual-property-rights/)
describes limited-purpose rights for contributed software under §9.2 of its
Directives, including implementation evaluation and conformance. Copyright
permission does not itself grant patent rights. This project's local synthetic
evaluation does not establish commercial licensing, distribution clearance or
standards certification. Keep reference source and test packages outside the
repository and review applicable terms for the intended use. Distribution
builds continue to exclude the EVS reference implementation.

## Build

```sh
cmake -S . -B out/build/fx -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DTELEPHONY_BUILD_PLUGIN=OFF \
  -DTELEPHONY_USE_EVS_FX=ON \
  -DTELEPHONY_EVS_FX_SOURCE_DIR=/path/to/official/c-code \
  -DTELEPHONY_BUILD_EVS_FX_REFERENCE_TOOLS=ON
cmake --build out/build/fx --parallel
```

The full official source set is compiled, including genuinely fixed-point
ACELP/TCX, pitch search, FD-CNG, VAD, RF and decoder state/concealment algorithms.
There are no parent algorithm replacements, float ACELP fallbacks, type-layout
shims, or duplicate-symbol suppression flags. A small generated namespace
header isolates shared names from AMR codecs without changing arithmetic.

The API transports G.192 frames entirely in memory, uses the reference
compact-payload reader with G.192 bit ordering, validates frame headers/lengths,
and preallocates bitstream/synthesis workspaces. It initializes the reference's
input/output frame lengths and advances decoder initialization state. API
options cover native EVS, AMR-WB IO, DTX, SC-VBR and RF; runtime reconfiguration
preserves encoder predictor/filter history.

## Reproducible synthetic differential regression

```sh
cmake --build out/build/fx --target \
  TelephonyEvsFxReferenceEncoder TelephonyEvsFxReferenceDecoder \
  TelephonyEvsFxReferenceAdapter
python3 tests/EvsFxDifferential.py \
  --encoder out/build/fx/TelephonyEvsFxReferenceEncoder \
  --decoder out/build/fx/TelephonyEvsFxReferenceDecoder \
  --adapter out/build/fx/TelephonyEvsFxReferenceAdapter
```

The script creates local deterministic tone/silence/resumption input. It compares
wrapper G.192 packets and decoded PCM byte-for-byte with the official CLI,
with CLI delay compensation disabled to match frame API semantics. It covers
80 configurations, all native bitrate families and sample rates, adaptive and
fixed DTX, SC-VBR, four RF offsets, nine AMR-WB IO bitrates at three input rates,
and both clean and deliberately lost speech frames. Two further profiles check
state-preserving native bitrate and native/AMR-WB IO encoding transitions,
with an actual rate change each frame. Identical API configuration reapplication
is a no-op; unlike the reference CLI profile reader, it does not reset internal
mode/SID state when the requested settings have not changed. Constant-rate
equivalence and repeated UI application are tested independently.
`TelephonyEvsFxApiRegression` separately checks malformed packets, unaligned
buffers, invalid configuration, zero input, and recovery after insufficient
output capacity. No external recordings are
used or uploaded.

These are wrapper/reference differential checks on generated input. They are
not a substitute for the complete TS 26.444 conformance suite, independent
reference outputs, or perceptual quality testing.

## Official conformance vectors remain separate

[TS 26.444 v16.5.0](https://www.etsi.org/deliver/etsi_ts/126400_126499/126444/16.05.00_60/)
provides a pointer file naming the matching BASOP package:
`26444_ce0_da0_e60_f40_g50-TestSeq_BASOP_26442.zip`, under
`ftp://ftp.3gpp.org/Specs/archive/26_series/26.444/test_sequences/`.
The pointer describes multi-gigabyte test packages. No such vector/audio package
is included in this repository or downloaded by its build. A full conformance
run requires obtaining the matching package, following its included instructions
and comparing all prescribed encoder/decoder outputs. Do not describe passing
synthetic tests as TS 26.444 certification.
