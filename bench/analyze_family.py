#!/usr/bin/env python3
"""
Our FEM-less board against an EBYTE module of the other chip family, right after a symbol's jump.
Rules in docs/PHY-notes.md (2026-09-24), declared before the capture.

The E220's preamble length is not known, so it is fitted: the length that makes the most header
landmarks common to both arms, required to beat the runner-up by a factor of two. Then, at the
five near-jump slices, E220 minus T-Deck.

  bench/analyze_family.py capture.json.gz [--a tdeck] [--b e220]
"""

import argparse
import json
import sys

from analyze_knobs import SLICES, at_slices, count
from analyze_scope_timing import analyse, best_preamble


def decide(points):
    lead, lag = count(points, -1), count(points, +1)
    return ("the EBYTE module leads after a jump" if lead >= 2 and lag < 2 else
            "no lead detected" if lead == 0 and lag == 0 else "mixed")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--a", default="tdeck")
    ap.add_argument("--b", default="e220")
    args = ap.parse_args()
    n, scores = best_preamble(args.captures, args.a, args.b, args.b)
    ranked = sorted(scores.values(), reverse=True)
    unique = ranked[0] >= 2 * max(ranked[1], 1)
    result = analyse(args.captures, a=args.a, b=args.b, preamble={args.b: n})
    pts = at_slices(result)
    out = {"preamble": n, "landmarks_by_length": scores, "preamble_unambiguous": unique,
           "near_jump": pts, "verdict": decide(pts) if unique else "invalid: preamble not determined"}
    for o in result:
        if o["offset_khz"] not in SLICES:
            out.setdefault("far_slices", {})[o["offset_khz"]] = {
                "header_shift": o["header_shift"],
                "landmarks": [(r["symbol"], r["b_minus_a_us"], r["ci_us"]) for r in o["landmarks"]]}
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    sys.exit(main())
