#!/usr/bin/env python3
"""
Symbol-level look at LoRa packets recorded with an RTL-SDR (rtl_sdr, 8-bit IQ).

Built to compare transmitters at the structure where our receivers misplace our data by one
symbol (PHY-notes, 2026-09-25): the preamble, the two sync symbols, the 2.25-symbol SFD and the
first data symbols. For every packet in a recording it reports:

- preamble length in symbols, and the dechirped tone of each preamble symbol - which moves with
  the carrier, so its spread is the carrier's drift through the preamble;
- the two sync symbols, in bins relative to the preamble;
- the SFD, from down-chirp dechirping: its tone, and how many down-chirp symbols it holds;
- where the data really starts against where the preamble grid says it should: the offset that
  makes the first data symbols dechirp cleanest.

Usage: sdr_lora.py capture.cu8 [--fs 2400000] [--sf 11] [--bw 500000]
numpy only.
"""
import argparse
import numpy as np

p = argparse.ArgumentParser()
p.add_argument("capture")
p.add_argument("--fs", type=float, default=2.4e6)
p.add_argument("--sf", type=int, default=11)
p.add_argument("--bw", type=float, default=500e3)
p.add_argument("--min-ms", type=float, default=100.0, help="shortest burst taken for a packet")
p.add_argument("--thresh-db", type=float, default=6.0, help="burst threshold over the median power")
a = p.parse_args()

SF, BW, FS = a.sf, a.bw, a.fs
N = 2 ** SF                 # bins (chips) per symbol
OS = 2                      # work at twice the bandwidth
FS2 = BW * OS
NS = N * OS                 # samples per symbol after resampling
T = N / BW                  # symbol time, s

raw = np.fromfile(a.capture, dtype=np.uint8).astype(np.float32) - 127.5
iq = raw[0::2] + 1j * raw[1::2]

def bursts(x, fs, min_ms):
    blk = int(fs / 1000)    # 1 ms: long enough to smooth a weak packet's power, short against one
    # Power inside the LoRa channel only: at 2.4 MS/s most of the noise lies outside it, and a
    # packet from the far room stands only a few dB over the wideband floor.
    n = len(x) // blk
    inband = np.abs(np.fft.fftfreq(blk, 1 / fs)) < BW / 2
    p = np.empty(n)
    for i in range(0, n, 1000):     # in slices: the whole capture at once is gigabytes of spectrum
        j = min(i + 1000, n)
        S = np.abs(np.fft.fft(x[i * blk:j * blk].reshape(-1, blk), axis=1)) ** 2
        p[i:j] = 10 * np.log10(S[:, inband].mean(axis=1) + 1e-9)
    on = p > np.median(p) + a.thresh_db
    out, start = [], None
    for i, v in enumerate(on):
        if v and start is None:
            start = i
        if not v and start is not None:
            if (i - start) >= min_ms:
                out.append((start * blk, i * blk))
            start = None
    return out

def resample(seg, fs_in, fs_out):
    """Exact-rate FFT resampling of a baseband segment already centred on DC."""
    n_in = len(seg)
    n_out = int(round(n_in * fs_out / fs_in))
    X = np.fft.fft(seg)
    h = n_out // 2
    Y = np.concatenate([X[:h], X[n_in - (n_out - h):]])
    return np.fft.ifft(Y) * (n_out / n_in)

n = np.arange(NS)
t = n / FS2
UP = np.exp(2j * np.pi * (-BW / 2 * t + 0.5 * (BW / T) * t ** 2))
DOWN = np.conj(UP)

def dechirp(x, start, ref):
    """Folded dechirped spectrum of one symbol window; returns (peak bin with fraction, peak share)."""
    w = x[start:start + NS]
    if len(w) < NS:
        return np.nan, 0.0
    S = np.abs(np.fft.fft(w * np.conj(ref))) ** 2
    F = S[:N] + S[N:]                     # fold the two images of a wrapped chirp
    k = int(np.argmax(F))
    l, c, r = F[(k - 1) % N], F[k], F[(k + 1) % N]
    frac = 0.5 * (l - r) / (l - 2 * c + r) if (l - 2 * c + r) != 0 else 0.0
    return (k + frac) % N, c / F.sum()

def rel(b, ref):
    """Bin difference wrapped to (-N/2, N/2]."""
    d = (b - ref) % N
    return d - N if d > N / 2 else d

for bi, (s0, s1) in enumerate(bursts(iq, FS, a.min_ms)):
    seg = iq[max(0, s0 - int(0.005 * FS)): s1 + int(0.005 * FS)]
    # coarse carrier: mean instantaneous frequency (a whole chirp averages to its centre)
    f0 = np.mean(np.diff(np.unwrap(np.angle(seg)))) * FS / (2 * np.pi)
    seg = seg * np.exp(-2j * np.pi * f0 * np.arange(len(seg)) / FS)
    x = resample(seg, FS, FS2)
    # preamble grid: the up-chirp correlation peak within the first symbols
    search = x[: 6 * NS]
    c = np.abs(np.correlate(search, UP, mode="valid"))
    first = int(np.argmax(c[: 2 * NS]))
    first %= NS
    # step the grid back to the burst start, then forward past any leading noise window
    while first - NS >= 0 and dechirp(x, first - NS, UP)[1] > 0.3:
        first -= NS
    while first + NS <= len(x) and dechirp(x, first, UP)[1] < 0.3:
        first += NS
    if first + NS > len(x):     # a burst of something else on the channel
        print(f"packet {bi + 1} at {s0 / FS:7.3f} s: no LoRa preamble in it")
        continue
    # preamble symbols: each within half a bin of the one before (the carrier may drift), with a
    # clean peak - a tone halfway between two bins still holds about 0.4 of the power
    pre_bins, k = [], 0
    while True:
        b, q = dechirp(x, first + k * NS, UP)
        if q < 0.3 or (pre_bins and abs(rel(b, pre_bins[-1])) > 0.5):
            break
        pre_bins.append(b)
        k += 1
    b0 = pre_bins[0]
    npre = len(pre_bins)
    pre = np.array([rel(b, b0) for b in pre_bins])
    # sync: the next two symbols, relative to the preamble's last tone
    ref = pre_bins[-1] if pre_bins else b0
    sync = [dechirp(x, first + (npre + i) * NS, UP) for i in range(2)]
    # SFD: down-chirp dechirping, symbol by symbol after the sync
    sfd = [dechirp(x, first + (npre + 2 + i) * NS, DOWN) for i in range(3)]
    # data start: where the up-chirp dechirping of the next 8 symbols is cleanest, searched around
    # the nominal npre + 2 + 2.25 symbols in 1/64-symbol steps
    nominal = first + int(round((npre + 2 + 2.25) * NS))
    best = None
    for off in range(-NS, NS + 1, NS // 64):
        qs = [dechirp(x, nominal + off + j * NS, UP)[1] for j in range(8)]
        score = float(np.mean(qs))
        if best is None or score > best[1]:
            best = (off, score)
    t_burst = s0 / FS
    # level against the 8-bit full scale: comparable between packets of one capture at one gain,
    # and only while nothing clips
    dbfs = 10 * np.log10(np.mean(np.abs(iq[s0:s1]) ** 2) / 127.5 ** 2)
    clip = np.mean(np.abs(raw[2 * s0:2 * s1]) >= 127.5)
    print(f"packet {bi + 1} at {t_burst:7.3f} s: level {dbfs:+.1f} dBFS (clipped {clip:.1%}), "
          f"carrier {f0 / 1e3:+7.2f} kHz, preamble {npre} symbols, "
          f"tone drift over it {pre.min():+.2f}..{pre.max():+.2f} bins ({(pre.max() - pre.min()) * BW / N:.0f} Hz)")
    print(f"    sync {rel(sync[0][0], ref):+8.2f} {rel(sync[1][0], ref):+8.2f} bins (peak share {sync[0][1]:.2f}, {sync[1][1]:.2f}); "
          f"SFD down-chirp tone {rel(sfd[0][0], ref):+8.2f} {rel(sfd[1][0], ref):+8.2f} (share {sfd[0][1]:.2f}, {sfd[1][1]:.2f}, third {sfd[2][1]:.2f}); "
          f"data starts {best[0] / NS:+.3f} symbols from nominal ({best[0] / FS2 * 1e6:+.0f} us), cleanliness {best[1]:.2f}")
