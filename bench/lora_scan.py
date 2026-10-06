#!/usr/bin/env python3
"""Find LoRa preambles too weak for an energy detector: dechirp every symbol-long window and
look for runs of windows whose spectral peak stands far over the median at a steady bin.

  bench/lora_scan.py capture.cu8      (rtl_sdr at 2.4 MS/s on the channel; SF11/BW500 as ours)

Each run prints its start, its length in symbols - an E22 sends 20, an E220 30 - and how far its
tone stands over the median bin, which compares two transmitters' levels within one capture."""
import sys
import numpy as np
if len(sys.argv) != 2:
    sys.exit(__doc__)
cap, FS, BW, N = sys.argv[1], 2.4e6, 500e3, 2048
FS2, NS = 1e6, 4096
raw = np.fromfile(cap, dtype=np.uint8).astype(np.float32) - 127.5
iq = raw[0::2] + 1j * raw[1::2]
t = np.arange(NS) / FS2
UP = np.exp(2j * np.pi * (-BW / 2 * t + 0.5 * (BW / (N / BW)) * t ** 2))
rows = []                                   # (time s, peak over median dB, peak bin)
chunk = int(FS)                             # 1 s at a time
for c0 in range(0, len(iq) - chunk, chunk):
    seg = iq[c0:c0 + chunk]
    X = np.fft.fft(seg); h = int(FS2) // 2
    x = np.fft.ifft(np.concatenate([X[:h], X[len(seg) - h:]])) * (FS2 / FS)
    for k in range(0, len(x) - NS, NS):
        S = np.abs(np.fft.fft(x[k:k + NS] * np.conj(UP))) ** 2
        F = S[:N] + S[N:]
        b = int(np.argmax(F))
        rows.append((c0 / FS + k / FS2, 10 * np.log10(F[b] / np.median(F)), b))
noise = np.median([r[1] for r in rows])
print(f"windows {len(rows)}, median peak-over-median {noise:.1f} dB (noise only)")
run = []
def flush():
    if len(run) >= 5:
        v = [r[1] for r in run]
        print(f"  preamble at {run[0][0]:7.3f} s: {len(run):2d} symbols, peak over median {np.median(v):5.1f} dB "
              f"({min(v):.1f}..{max(v):.1f}), bin {run[0][2]}")
for r in rows:
    if r[1] > noise + 6 and run and abs((r[2] - run[-1][2] + N // 2) % N - N // 2) <= 1:
        run.append(r)
    else:
        flush()
        run[:] = [r] if r[1] > noise + 6 else []
flush()
