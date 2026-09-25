#!/usr/bin/env python3
"""
Does the T-Deck lose the Heltec's packets according to the fractional part of their frequency
offset? Rules in docs/PHY-notes.md (2026-09-25), declared before the run.

The T-Deck's tuning (`TUNE <Hz>`) steps through one 244 Hz bin in palindromic blocks while the
Heltec and the E22 transmit. Per state: each arm's delivery at the T-Deck, and the median
RegFreqError of the Heltec's receptions, which shows the step reached the air. The test is the
chi-square of the Heltec's per-state delivery, with p from permuting state labels over blocks.

  bench/analyze_frac.py run.json [--rx tdeck] [--arm heltec] [--ref e22]
  bench/analyze_frac.py --self-check
"""

import argparse
import json
import random
import statistics as st
import sys

from analyze_fail import parse


def chi2(groups):
    """Pearson chi-square of homogeneity for {state: (hits, n)}."""
    hits = sum(h for h, _ in groups.values())
    total = sum(n for _, n in groups.values())
    p = hits / total
    if p in (0.0, 1.0):
        return 0.0
    return sum((h - n * p) ** 2 / (n * p) + (h - n * p) ** 2 / (n * (1 - p))
               for h, n in groups.values() if n)


def group(blocks, labels):
    g = {}
    for lab, (_, h, n) in zip(labels, blocks):
        gh, gn = g.get(lab, (0, 0))
        g[lab] = (gh + h, gn + n)
    return g


def perm_p(blocks, n_perm=20000, seed=1):
    """blocks: [(state, hits, n)]; p of the observed chi-square among label permutations."""
    labels = [b[0] for b in blocks]
    observed = chi2(group(blocks, labels))
    rng = random.Random(seed)
    count = 0
    for _ in range(n_perm):
        rng.shuffle(labels)
        if chi2(group(blocks, labels)) >= observed - 1e-9:
            count += 1
    return observed, (count + 1) / (n_perm + 1)


def hz(state):
    return int(state.split()[1])


def analyse(run, rx, arm, ref, n_perm=20000):
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {int(x) for x in run["seen"][rx]}
    state = {int(k): v for k, v in run["neighbour"].items()}
    argv = run["argv"]
    size = int(argv[argv.index("--block") + 1])
    starts = {int(k): v[0] for k, v in run["tx_times"].items()}
    ok, fails = parse(run["log"][rx], run["log_t"][rx], starts)

    per = {}                      # state -> {arm: [hits, n]}, and fe of the arm's receptions
    fe = {}
    blk = {}                      # block index -> [state, hits, n] for the tested arm
    for s, w in sent.items():
        if w not in (arm, ref) or s not in state:
            continue
        d = per.setdefault(state[s], {arm: [0, 0], ref: [0, 0]})
        d[w][0] += s in seen
        d[w][1] += 1
        if w == arm:
            b = blk.setdefault((s - 1) // size, [state[s], 0, 0])
            b[1] += s in seen
            b[2] += 1
            if s in ok and "fe" in ok[s]:
                fe.setdefault(state[s], []).append(ok[s]["fe"])
    blocks = [tuple(v) for _, v in sorted(blk.items())]
    stat, p = perm_p(blocks, n_perm)
    rates = {k: v[arm][0] / v[arm][1] for k, v in per.items() if v[arm][1]}
    spread = 100 * (max(rates.values()) - min(rates.values()))

    base = st.median(fe.get("TUNE 0", [0]))
    med = {k: st.median(v) for k, v in fe.items()}
    sign = 1 if med.get("TUNE 244", base + 244) - base > 0 else -1
    tracking = {k: round(m - base - sign * hz(k), 1) for k, m in med.items()}
    valid = (all(v[ref][1] and v[ref][0] >= 0.9 * v[ref][1] for v in per.values())
             and all(abs(d) <= 15 for d in tracking.values()) and len(per) == 9)
    out = {
        "states": {k: {"heltec": f"{v[arm][0]}/{v[arm][1]}", "e22": f"{v[ref][0]}/{v[ref][1]}",
                       "fe_median_hz": med.get(k), "fraction_of_bin": round((med[k] / 244.14) % 1, 3)
                       if k in med else None}
                   for k, v in sorted(per.items(), key=lambda kv: hz(kv[0]))},
        "fe_sign": sign, "fe_tracking_error_hz": tracking,
        "chi2": round(stat, 2), "perm_p": round(p, 5), "spread_points": round(spread, 1),
        "tune0_minus_tune244_points": round(100 * (rates.get("TUNE 0", 0) - rates.get("TUNE 244", 0)), 1),
        "arm_fail_kinds": {k: sum(f["kind"] == k and sent.get(f["seq"]) == arm for f in fails)
                           for k in ("hdr", "crc")},
        "other_receivers": {r: {w: f"{sum(s in set(map(int, run['seen'][r])) for s in sent if sent[s] == w)}"
                                   f"/{sum(1 for s in sent if sent[s] == w)}" for w in (arm, ref)}
                            for r in run["seen"] if r != rx},
        "valid": valid,
    }
    out["verdict"] = ("invalid" if not valid else
                      "depends on the fractional offset" if p < 0.001 and spread >= 25 else
                      "no dependence detected" if p >= 0.01 and spread < 20 else "inconclusive")
    return out


def self_check():
    assert chi2({"a": (8, 10), "b": (8, 10)}) == 0.0
    assert chi2({"a": (10, 10), "b": (0, 10)}) == 20.0
    flat = [("a", 8, 10), ("b", 8, 10)] * 10
    assert perm_p(flat, 200)[1] == 1.0
    split = [("a", 10, 10), ("b", 0, 10)] * 10
    assert perm_p(split, 2000)[1] < 0.001
    assert hz("TUNE 214") == 214
    print("analyze_frac: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--rx", default="tdeck")
    ap.add_argument("--arm", default="heltec")
    ap.add_argument("--ref", default="e22")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    print(json.dumps(analyse(json.load(open(args.run)), args.rx, args.arm, args.ref), indent=1))


if __name__ == "__main__":
    sys.exit(main())
