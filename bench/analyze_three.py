#!/usr/bin/env python3
"""
Heltec, E22 and E220 on one scope: which of them differs right after a symbol's jump?
Rules in docs/PHY-notes.md (2026-09-24), declared before the capture.

Pairs, each at the five near-jump slices, counting only slices where both arms qualify (preamble
chains in at least 75 % of their captures): E22 minus Heltec (positive control), E220 minus Heltec,
E22 minus E220. The E220's preamble length is fitted as in analyze_family.py.

  bench/analyze_three.py capture.json.gz
"""

import json
import sys
from pathlib import Path

from analyze_knobs import at_slices, count
from analyze_rssi import analyze, load_captures
from analyze_scope_timing import analyse, best_preamble


def qualified(path):
    """{(offset, arm): bool} by the 2026-09-22 criterion, 75 % of captures with a chain."""
    out = {}
    for o in analyze(load_captures([Path(path)]))["offsets"]:
        for arm, t in o["transmitters"].items():
            out[(o["offset_khz"], arm)] = t["captures"] > 0 and t["chains"] >= 0.75 * t["captures"]
    return out


def pair(path, a, b, pre, q):
    pts = at_slices(analyse([path], a=a, b=b, preamble=pre))
    return {f: (p if q.get((f, a)) and q.get((f, b)) else None) for f, p in pts.items()}


def main():
    path = sys.argv[1]
    q = qualified(path)
    n, scores = best_preamble([path], "heltec", "e220", "e220")
    ranked = sorted(scores.values(), reverse=True)
    pre = {"e220": n}
    control = pair(path, "heltec", "e22", pre, q)
    e220 = pair(path, "heltec", "e220", pre, q)
    ebyte = pair(path, "e220", "e22", pre, q)
    valid = count(control, -1) >= 2 and ranked[0] >= 2 * max(ranked[1], 1)
    lead = count(e220, -1)
    out = {
        "e220_preamble": n, "preamble_unambiguous": ranked[0] >= 2 * max(ranked[1], 1),
        "qualified": {f"{f}/{a}": v for (f, a), v in sorted(q.items())},
        "e22_minus_heltec": control, "e220_minus_heltec": e220, "e22_minus_e220": ebyte,
        "valid": valid,
        "q1_e220_vs_heltec": ("invalid" if not valid else
                              "the E220 leads like the E22" if lead >= 2 and count(e220, +1) < 2 else
                              "no lead detected" if lead == 0 and count(e220, +1) == 0 else "mixed"),
        "q2_e22_vs_e220": ("invalid" if not valid else
                           "the EBYTE modules differ" if count(ebyte, -1) >= 2 or count(ebyte, +1) >= 2
                           else "no difference detected"),
    }
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    sys.exit(main())
