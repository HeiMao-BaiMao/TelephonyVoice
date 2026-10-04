#!/usr/bin/env python3
"""Synthetic byte-for-byte wrapper/reference regression; not TS26.444 certification."""
import argparse
import array
import math
from pathlib import Path
import struct
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--encoder', required=True, type=Path)
p.add_argument('--decoder', required=True, type=Path)
p.add_argument('--adapter', required=True, type=Path)
p.add_argument('--work-dir', type=Path)
a = p.parse_args()
a.encoder, a.decoder, a.adapter = (x.resolve() for x in (a.encoder, a.decoder, a.adapter))
owned = tempfile.TemporaryDirectory(prefix='telephony-fx-diff-') if a.work_dir is None else None
root = a.work_dir or Path(owned.name)
root.mkdir(parents=True, exist_ok=True)

def run(args):
    r = subprocess.run([str(x) for x in args], capture_output=True, timeout=120)
    if r.returncode:
        raise RuntimeError(f'{args}: {r.returncode}\n{r.stdout.decode(errors="replace")}\n{r.stderr.decode(errors="replace")}')

def compare(ref, actual, description):
    x, y = ref.read_bytes(), actual.read_bytes()
    if x != y:
        first = next((i for i, (u, v) in enumerate(zip(x, y)) if u != v), min(len(x), len(y)))
        raise AssertionError(f'{description}: {len(x)} vs {len(y)} bytes; first mismatch at {first}')

for sr in (8000, 16000, 32000, 48000):
    samples = array.array('h')
    rng = 0x13579BDF
    for frame in range(120):
        for i in range(sr // 50):
            t = (frame * (sr // 50) + i) / sr
            v = 0 if 25 <= frame < 95 else int(10000 * math.sin(2 * math.pi * 213 * t) + 2500 * math.sin(2 * math.pi * 677 * t))
            if frame >= 95:
                # Resumption adds high-band content; the final frames add
                # deterministic broadband noise to exercise SWB/FB paths.
                v += int(2200 * math.sin(2 * math.pi * (sr * 0.38) * t))
            if frame >= 108:
                rng = (1664525 * rng + 1013904223) & 0xffffffff
                v += ((rng >> 16) - 32768) // 10
            samples.append(v)
    (root / f'input-{sr}.pcm').write_bytes(samples.tobytes())

cases = []
for sr, bw in ((8000, 0), (16000, 1), (32000, 2), (48000, 3)):
    for br in (5900,7200,8000,9600,13200,16400,24400,32000,48000,64000,96000,128000):
        if sr == 8000 and br > 24400: continue
        effective_bw = min(bw, 1 if br < 9600 else 2 if br < 16400 else 3)
        cases.append((sr,br,effective_bw,1,0,0,0,0))
for offset in (2,3,5,7):
    cases.append((32000,13200,2,1,8,1,offset,0))
for sr in (16000,32000,48000):
    for br in (6600,8850,12650,14250,15850,18250,19850,23050,23850):
        cases.append((sr,br,1,1,8,0,0,1))
for br in (7200,9600,13200,24400,64000,128000):
    cases.append((48000,br,1 if br < 9600 else 2 if br < 16400 else 3,0,0,0,0,0))

for index, config in enumerate(cases):
    sr, br, bw, dtx, sid, rf, offset, io = config
    source = root / f'input-{sr}.pcm'
    ref, ours = root / 'ref.192', root / 'ours.192'
    flags = ['-q', '-no_delay_cmp', '-max_band', ('NB','WB','SWB','FB')[bw]]
    if dtx: flags += ['-dtx', str(sid)]
    if rf: flags += ['-rf', 'HI', str(offset)]
    run([a.encoder, *flags, br, sr // 1000, source, ref])
    run([a.adapter, 'encode', *config, source, ours])
    compare(ref, ours, f'encode {config}')
    # Both clean and loss-bearing streams are evaluated. SID and NO_DATA are
    # generated naturally; only speech packets are deliberately erased.
    for lost in (False, True):
        if lost:
            data = bytearray(ref.read_bytes()); pos = 0; frame = 0
            while pos < len(data):
                bits = struct.unpack_from('=H', data, pos + 2)[0]
                if frame in (7,8,14,98,99) and bits > 56:
                    struct.pack_into('=H', data, pos, 0x6b20)
                pos += 4 + bits * 2; frame += 1
            (root / 'loss.192').write_bytes(data)
            encoded = root / 'loss.192'
        else: encoded = ref
        expected, actual = root / 'ref.pcm', root / 'ours.pcm'
        run([a.decoder, '-q', '-no_delay_cmp', sr // 1000, encoded, expected])
        run([a.adapter, 'decode', *config, encoded, actual])
        compare(expected, actual, f'decode {config}, loss={lost}')
    print(f'PASS {index + 1}/{len(cases)} {config}', flush=True)
print(f'PASS: {len(cases)} configurations; synthetic encoding, DTX, RF, IO and lost-frame differential checks')

# Bitrate-profile transitions use the existing encoder state (no reconstruction).
# Every profile frame requests an actual change: the CLI profile reader resets
# its mode even for repeated rates, whereas identical API/UI apply is a no-op.
# Constant-rate equivalence and repeated UI apply are tested independently.
for label, rates in (
    ('native', (7200,9600,13200,16400,24400,32000,48000,64000,96000,128000)),
    ('native_io', (13200,12650,23850,9600,6600,24400,8850,32000,23050,128000)),
):
    profile = root / 'rates.bin'
    profile.write_bytes(b''.join(struct.pack('=i', rates[frame % len(rates)]) for frame in range(120)))
    cfg = (48000, rates[0], 1, 1, 8, 0, 0, 0)
    ref, ours = root / 'profile-ref.192', root / 'profile-ours.192'
    run([a.encoder, '-q', '-no_delay_cmp', '-max_band', 'WB', '-dtx', 8,
         profile, 48, root / 'input-48000.pcm', ref])
    run([a.adapter, 'encode', *cfg, root / 'input-48000.pcm', ours, profile])
    compare(ref, ours, f'profile encode {label}')
    if label == 'native':
        run([a.decoder, '-q', '-no_delay_cmp', 48, ref, root / 'profile-ref.pcm'])
        run([a.adapter, 'decode', *cfg, ours, root / 'profile-ours.pcm'])
        compare(root / 'profile-ref.pcm', root / 'profile-ours.pcm', 'profile decode')
    print(f'PASS runtime profile {label}', flush=True)
