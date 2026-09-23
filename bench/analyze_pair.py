#!/usr/bin/env python3
"""
Do two receivers lose the same packets? Rules declared in docs/PHY-notes.md before the run.

For every packet of the tested arm: delivered at receiver A, delivered at receiver B. The
decision uses the conditional delivery shares P(B|A) and P(A|B) against the marginals; a
Mantel-Haenszel odds ratio over time blocks is reported alongside, so that drift shared by
both receivers cannot pass for a per-transmission effect unnoticed.

  bench/analyze_pair.py run.json --arm tdeck --a e22 --b e220b [--ref e220] [--block 25]
"""

import argparse
import json
import sys


def table(pairs):
    """pairs: [(a, b)] booleans -> n11, n10, n01, n00."""
    n11 = sum(a and b for a, b in pairs)
    n10 = sum(a and not b for a, b in pairs)
    n01 = sum(b and not a for a, b in pairs)
    return n11, n10, n01, len(pairs) - n11 - n10 - n01


def mantel_haenszel(blocks):
    """Common odds ratio over 2x2 tables; None when no block carries information."""
    num = den = 0.0
    for n11, n10, n01, n00 in blocks:
        n = n11 + n10 + n01 + n00
        if n:
            num += n11 * n00 / n
            den += n10 * n01 / n
    if den == 0:
        return None if num == 0 else float("inf")
    return num / den


def decide(pairs, min_hits=8):
    n11, n10, n01, n00 = table(pairs)
    n = len(pairs)
    na, nb = n11 + n10, n11 + n01
    out = {"n": n, "A": na, "B": nb, "both": n11,
           "expected_both_if_independent": round(na * nb / n, 1) if n else None}
    if na < min_hits or nb < min_hits:
        out["verdict"] = "underpowered"
        return out
    pa, pb = na / n, nb / n
    pb_a, pa_b = n11 / na, n11 / nb
    out.update({"P(A)": round(pa, 3), "P(B)": round(pb, 3),
                "P(B|A)": round(pb_a, 3), "P(A|B)": round(pa_b, 3)})
    if pb_a >= 0.6 and pa_b >= 0.6:
        out["verdict"] = "same packets"
    elif pb_a <= pb + 0.2 and pa_b <= pa + 0.2:
        out["verdict"] = "near independent"
    else:
        out["verdict"] = "partial"
    return out


def analyse(run, arm, a, b, ref, block, state=None):
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {k: {int(x) for x in v} for k, v in run["seen"].items()}
    # Only packets sent while a --toggle receiver was in this state, when asked for.
    nb = {int(k): v for k, v in run.get("neighbour", {}).items()}
    seqs = sorted(s for s, w in sent.items() if w == arm and (state is None or nb.get(s) == state))
    pairs = [(s in seen[a], s in seen[b]) for s in seqs]
    res = decide(pairs)
    blocks = [table(pairs[i:i + block]) for i in range(0, len(pairs), block)]
    res["blocks_both_A_B"] = [(t[0], t[0] + t[1], t[0] + t[2]) for t in blocks]
    mh = mantel_haenszel(blocks)
    res["MH_odds_ratio"] = None if mh is None else round(mh, 2)
    if ref:
        refs = [s for s, w in sent.items() if w == ref and (state is None or nb.get(s) == state)]
        res["ref"] = {rx: f"{sum(s in seen[rx] for s in refs)}/{len(refs)}" for rx in seen}
    res["arm_at_each_rx"] = {rx: f"{sum(s in seen[rx] for s in seqs)}/{len(seqs)}" for rx in seen}
    return res


def self_check():
    same = [(True, True)] * 20 + [(False, False)] * 180
    assert decide(same)["verdict"] == "same packets"
    indep = ([(True, True)] * 2 + [(True, False)] * 18 + [(False, True)] * 18
             + [(False, False)] * 162)
    assert decide(indep)["verdict"] == "near independent"
    assert decide([(True, True)] * 3 + [(False, False)] * 197)["verdict"] == "underpowered"
    assert mantel_haenszel([(2, 18, 18, 162)]) == 1.0
    print("analyze_pair: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--arm", default="tdeck")
    ap.add_argument("--a", default="e22")
    ap.add_argument("--b", default="e220b")
    ap.add_argument("--ref", default="e220")
    ap.add_argument("--block", type=int, default=25)
    ap.add_argument("--state", choices=["SLEEP", "WAKE"], help="only packets sent in this --toggle state")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    print(json.dumps(analyse(json.load(open(args.run)), args.arm, args.a, args.b, args.ref, args.block,
                             args.state), indent=1))


if __name__ == "__main__":
    sys.exit(main())
