#!/usr/bin/env python3
"""
Does a sleeping neighbour change a receiver's delivery? Rules in docs/PHY-notes.md, declared
before the run.

The neighbour's state was switched in blocks (ABBA); adjacent blocks form pairs holding one of
each state, so the test flips signs within pairs rather than treating packets as independent.

  bench/analyze_neighbour.py run.json [--arm tdeck] [--rx e22] [--ref e220] [--neighbour e220b]
"""

import argparse
import json
import math
import random
import sys


def blocks(seqs, state, size):
    """The switching blocks as run: consecutive groups of `size` tested packets, each one state.

    Not runs of equal state - ABBA puts two blocks of one state side by side, and merging them
    would pair a 10-packet block with a 20-packet one.
    """
    out = []
    for i in range(0, len(seqs), size):
        grp = seqs[i:i + size]
        states = {state[s] for s in grp}
        assert len(states) == 1, f"block at {grp[0]} spans a switch"
        out.append((states.pop(), grp))
    return out


def paired_permutation(diffs, trials=200_000, seed=1):
    """Two-sided sign-flip p-value for the sum of per-pair differences."""
    obs = abs(sum(diffs))
    if obs == 0:
        return 1.0
    rng = random.Random(seed)
    hits = sum(abs(sum(d if rng.random() < 0.5 else -d for d in diffs)) >= obs for _ in range(trials))
    return hits / trials


def analyse(run, arm, rx, ref, neighbour, size):
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {k: {int(x) for x in v} for k, v in run["seen"].items()}
    state = {int(k): v for k, v in run["neighbour"].items()}
    tested = sorted(s for s, w in sent.items() if w == arm and s in state)
    refs = sorted(s for s, w in sent.items() if w == ref and s in state)
    out = {}
    for st in ("SLEEP", "WAKE"):
        t = [s for s in tested if state[s] == st]
        r = [s for s in refs if state[s] == st]
        out[st] = {"n": len(t), rx: sum(s in seen[rx] for s in t),
                   f"ref@{rx}": f"{sum(s in seen[rx] for s in r)}/{len(r)}",
                   f"ref@{neighbour}": f"{sum(s in seen[neighbour] for s in r)}/{len(r)}",
                   "others": {k: sum(s in v for s in t) for k, v in seen.items() if k != rx}}
    bl = blocks(tested, state, size)
    diffs = []
    for i in range(0, len(bl) - 1, 2):
        (a, sa), (b, sb) = bl[i], bl[i + 1]
        if a == b:
            continue
        sleep, wake = (sa, sb) if a == "SLEEP" else (sb, sa)
        diffs.append(sum(s in seen[rx] for s in sleep) - sum(s in seen[rx] for s in wake))
    ps = out["SLEEP"][rx] / out["SLEEP"]["n"] if out["SLEEP"]["n"] else 0.0
    pw = out["WAKE"][rx] / out["WAKE"]["n"] if out["WAKE"]["n"] else 0.0
    se = math.sqrt(ps * (1 - ps) / max(out["SLEEP"]["n"], 1) + pw * (1 - pw) / max(out["WAKE"]["n"], 1))
    out["sleep_minus_wake_points"] = round(100 * (ps - pw), 1)
    out["approx_95ci_points"] = [round(100 * (ps - pw - 1.96 * se), 1), round(100 * (ps - pw + 1.96 * se), 1)]
    out["block_pairs"] = len(diffs)
    out["pair_diffs"] = diffs
    out["paired_permutation_p"] = round(paired_permutation(diffs), 4)
    ref_ok = all(int(out[st][f"ref@{rx}"].split("/")[0]) >= 0.9 * int(out[st][f"ref@{rx}"].split("/")[1])
                 for st in ("SLEEP", "WAKE"))
    asleep_silent = out["SLEEP"][f"ref@{neighbour}"].startswith("0/")
    awake = out["WAKE"][f"ref@{neighbour}"].split("/")
    awake_ok = int(awake[0]) >= 0.9 * int(awake[1])
    out["valid"] = ref_ok and asleep_silent and awake_ok
    if not out["valid"]:
        out["verdict"] = "invalid"
    elif abs(out["sleep_minus_wake_points"]) >= 8 and out["paired_permutation_p"] < 0.05:
        out["verdict"] = "effect"
    else:
        out["verdict"] = "no effect detected at this sensitivity"
    return out


def self_check():
    assert paired_permutation([0, 0, 0]) == 1.0
    assert paired_permutation([3] * 12) < 0.001
    st = {1: "SLEEP", 2: "SLEEP", 3: "WAKE", 4: "WAKE", 5: "WAKE", 6: "WAKE", 7: "SLEEP", 8: "SLEEP"}
    # ABBA: the two WAKE blocks stay two blocks, so every pair is one block of each state.
    assert [b[0] for b in blocks(sorted(st), st, 2)] == ["SLEEP", "WAKE", "WAKE", "SLEEP"]
    print("analyze_neighbour: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--arm", default="tdeck")
    ap.add_argument("--rx", default="e22")
    ap.add_argument("--ref", default="e220")
    ap.add_argument("--neighbour", default="e220b")
    ap.add_argument("--block", type=int, default=10, help="tested packets per switching block")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    print(json.dumps(analyse(json.load(open(args.run)), args.arm, args.rx, args.ref, args.neighbour,
                             args.block), indent=1))


if __name__ == "__main__":
    sys.exit(main())
