#!/usr/bin/env python3
"""
Test C of the 2026-09-23 series (docs/PHY-notes.md), declared before the run.

  --freq: C1, RegFreqError per source as logged by an SX_FREQ_ERROR receiver.
  default: C2, the receiver's delivery by source and by its own tuning, sign-flip test over the
  switching blocks; "follows the offset" needs the tested source to rise and the reference to
  fall by --points each, with p < 0.05.

  bench/analyze_tune.py run.json --freq [--rx tdeck]
  bench/analyze_tune.py run.json [--rx tdeck] [--tested heltec] [--ref e22] [--block 10]
"""

import argparse
import json
import re
import statistics
import sys

from analyze_neighbour import blocks, paired_permutation

FREQ = re.compile(r"freq error ([+-]?\d+) Hz")
RX = re.compile(r"bridge: rx \d+ bytes(?:, rssi -?\d+)?: (\d{8})")


def freq_by_seq(lines):
    out, last = {}, None
    for line in lines:
        m = FREQ.search(line)
        if m:
            last = int(m[1])
        m = RX.search(line)
        if m:
            if last is not None:
                out[int(m[1])] = last
            last = None
    return out


def freq_report(run, rx):
    sent = {int(k): v for k, v in run["sent"].items()}
    fe = freq_by_seq(run["log"][rx])
    out = {}
    for arm in sorted(set(sent.values())):
        vals = [fe[s] for s, w in sent.items() if w == arm and s in fe]
        out[arm] = {"n": len(vals), "sent": sum(w == arm for w in sent.values())}
        if vals:
            out[arm].update(median=statistics.median(vals), min=min(vals), max=max(vals))
    return out


def tune_report(run, rx, tested, ref, size, points):
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {k: {int(x) for x in v} for k, v in run["seen"].items()}
    state = {int(k): v for k, v in run["neighbour"].items()}
    names = sorted(set(state.values()))
    zero = next(n for n in names if n.split()[-1] == "0")
    off = next(n for n in names if n != zero)
    out = {"zero": zero, "offset": off}
    for arm in (tested, ref):
        seqs = sorted(s for s, w in sent.items() if w == arm and s in state)
        by = {st: [s for s in seqs if state[s] == st] for st in (zero, off)}
        rate = {st: sum(s in seen[rx] for s in v) / len(v) for st, v in by.items() if v}
        diffs = []
        bl = blocks(seqs, state, size)
        for i in range(0, len(bl) - 1, 2):
            (a, sa), (b, sb) = bl[i], bl[i + 1]
            if a != b:
                o, z = (sa, sb) if a == off else (sb, sa)
                diffs.append(sum(s in seen[rx] for s in o) - sum(s in seen[rx] for s in z))
        out[arm] = {st: f"{sum(s in seen[rx] for s in v)}/{len(v)}" for st, v in by.items()}
        out[arm]["offset_minus_zero_points"] = round(100 * (rate[off] - rate[zero]), 1)
        out[arm]["p"] = round(paired_permutation(diffs), 4)
        out[arm]["elsewhere"] = {k: sum(s in v for s in seqs) for k, v in seen.items() if k != rx}
    rise = out[tested]["offset_minus_zero_points"] >= points and out[tested]["p"] < 0.05
    fall = -out[ref]["offset_minus_zero_points"] >= points and out[ref]["p"] < 0.05
    moved = (abs(out[tested]["offset_minus_zero_points"]) >= points
             or abs(out[ref]["offset_minus_zero_points"]) >= points)
    out["verdict"] = ("follows the offset" if rise and fall
                      else "not tied to the offset" if not moved else "mixed")
    return out


def self_check():
    lines = ["freq error -30 Hz (raw 60)", "I (1) bridge: rx 38 bytes: 00000001AAAA...",
             "I (2) bridge: rx 38 bytes: 00000002AAAA...", "freq error +8200 Hz (raw 1)",
             "I (3) bridge: rx 38 bytes: 00000003AAAA..."]
    # A reception without its own freq line must not inherit the previous packet's value.
    assert freq_by_seq(lines) == {1: -30, 3: 8200}
    print("analyze_tune: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--freq", action="store_true")
    ap.add_argument("--rx", default="tdeck")
    ap.add_argument("--tested", default="heltec")
    ap.add_argument("--ref", default="e22")
    ap.add_argument("--block", type=int, default=10)
    ap.add_argument("--points", type=float, default=8)
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    run = json.load(open(args.run))
    res = freq_report(run, args.rx) if args.freq else tune_report(run, args.rx, args.tested, args.ref,
                                                                   args.block, args.points)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    sys.exit(main())
