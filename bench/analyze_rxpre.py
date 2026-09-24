#!/usr/bin/env python3
"""
Does our receiver, told to expect a module's preamble length, start refusing our packets?
Rules in docs/PHY-notes.md (2026-09-24), declared before the run.

Delivery at the toggled receiver by transmitter and by its RXPRE state; each state compared with
the baseline state by a bootstrap over switching blocks (resampling whole blocks, so packets are
not treated as independent).

  bench/analyze_rxpre.py run.json [--rx tdeck] [--tested heltec] [--refs e22 e220] [--base "RXPRE 8"]
"""

import argparse
import itertools
import json
import random
import sys


def blocks_of(seqs, state):
    """Consecutive runs of one state as the switching blocks (the toggle changes state per block;
    a palindrome's doubled middle block is one block of twice the length, which is fine here)."""
    return [(st, list(g)) for st, g in itertools.groupby(seqs, key=lambda s: state[s])]


def boot_diff(blocks_x, blocks_b, seen, trials=20000, level=99.0, seed=1):
    rng = random.Random(seed)

    def rate(bl):
        n = sum(len(b) for b in bl)
        return sum(s in seen for b in bl for s in b) / n if n else float("nan")

    d = []
    for _ in range(trials):
        x = [rng.choice(blocks_x) for _ in blocks_x]
        b = [rng.choice(blocks_b) for _ in blocks_b]
        d.append(rate(x) - rate(b))
    d.sort()
    tail = (100 - level) / 200
    return rate(blocks_x) - rate(blocks_b), [d[int(tail * trials)], d[int((1 - tail) * trials) - 1]]


def analyse(run, rx, tested, refs, base):
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {k: {int(x) for x in v} for k, v in run["seen"].items()}
    state = {int(k): v for k, v in run["neighbour"].items()}
    seqs = sorted(s for s in sent if s in state)
    bl = blocks_of(seqs, state)
    states = sorted({st for st, _ in bl})
    out = {"delivery": {}, "comparisons": {}}
    for st in states:
        out["delivery"][st] = {arm: f"{sum(s in seen[rx] for s in seqs if state[s] == st and sent[s] == arm)}/"
                                    f"{sum(1 for s in seqs if state[s] == st and sent[s] == arm)}"
                               for arm in [tested] + refs}
        out["delivery"][st]["others"] = {k: sum(s in v for s in seqs if state[s] == st and sent[s] == tested)
                                         for k, v in seen.items() if k != rx}
    tb = {st: [[s for s in b if sent[s] == tested] for s2, b in bl if s2 == st] for st in states}
    for st in states:
        if st == base:
            continue
        diff, ci = boot_diff(tb[st], tb[base], seen[rx])
        ref_ok = all(int(out["delivery"][st][r].split("/")[0]) >= 0.9 * int(out["delivery"][st][r].split("/")[1])
                     for r in refs)
        verdict = ("reproduces the modules' selectivity" if diff <= -0.30 and ci[1] < 0 and ref_ok else
                   "no effect detected" if abs(diff) < 0.15 else "partial")
        out["comparisons"][f"{st} vs {base}"] = {"tested_diff_points": round(100 * diff, 1),
                                                "ci99_points": [round(100 * c, 1) for c in ci],
                                                "refs_at_90pct": ref_ok, "verdict": verdict}
    return out


def self_check():
    seen = {1, 2, 3, 4}
    diff, ci = boot_diff([[1, 2], [3, 4]], [[5, 6], [7, 8]], seen, trials=500)
    assert diff == 1.0 and ci[0] > 0
    st = {1: "A", 2: "A", 3: "B", 4: "B", 5: "A"}
    assert [b[0] for b in blocks_of([1, 2, 3, 4, 5], st)] == ["A", "B", "A"]
    print("analyze_rxpre: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--rx", default="tdeck")
    ap.add_argument("--tested", default="heltec")
    ap.add_argument("--refs", nargs="+", default=["e22", "e220"])
    ap.add_argument("--base", default="RXPRE 8")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    print(json.dumps(analyse(json.load(open(args.run)), args.rx, args.tested, args.refs, args.base), indent=1))


if __name__ == "__main__":
    sys.exit(main())
