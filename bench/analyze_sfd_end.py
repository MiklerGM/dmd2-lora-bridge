#!/usr/bin/env python3
"""
The end of the SFD and the start of the data, our transmitter against the E22's, with both carriers
at the same offset. Rules in docs/PHY-notes.md (2026-09-25), declared before the capture.

On a slice at f kHz the closing quarter downchirp (+250 to +125 kHz) crosses at 12.0 + (250 - f)/500
symbols after our first preamble symbol, and the first data symbol (value 348, from -165.04 kHz)
at 12.25 + (f + 165.04)/500. The landmark nearest each, within 0.05 symbol, gives E22 minus Heltec.

  bench/analyze_sfd_end.py capture.json.gz [...]
"""

import json
import sys

import numpy as np

from analyze_scope_timing import analyse

QUARTER = (140, 170, 200, 230)
DATA = (-150, -120)
F0 = -165.04


def target(f):
    return 12.0 + (250 - f) / 500 if f in QUARTER else 12.25 + (f - F0) / 500


def nearest(rows, sym):
    best = min(rows, key=lambda r: abs(r["symbol"] - sym), default=None)
    return best if best and abs(best["symbol"] - sym) <= 0.05 else None


def verdict(points):
    hits = [p for p in points if p and abs(p["b_minus_a_us"]) >= 20 and (p["ci_us"][0] > 0 or p["ci_us"][1] < 0)]
    signs = {np.sign(p["b_minus_a_us"]) for p in hits}
    return hits, len(signs) == 1


def main():
    result = analyse(sys.argv[1:], a="heltec", b="e22")
    by = {o["offset_khz"]: o for o in result}
    out = {"quarter": {}, "data": {}, "others": []}
    for f in QUARTER + DATA:
        o = by.get(f)
        hit = nearest(o["landmarks"], target(f)) if o else None
        (out["quarter"] if f in QUARTER else out["data"])[f] = {"predicted_symbol": round(target(f), 3), "landmark": hit}
    for o in result:
        for r in o["landmarks"]:
            if not any(r is v["landmark"] for d in (out["quarter"], out["data"]) for v in d.values()):
                out["others"].append(abs(r["b_minus_a_us"]))
    med = float(np.median(out["others"])) if out["others"] else None
    out["others_median_abs_us"] = med
    q_hits, q_same = verdict([v["landmark"] for v in out["quarter"].values()])
    d_hits, d_same = verdict([v["landmark"] for v in out["data"].values()])
    valid = med is not None and med <= 15
    out["valid"] = valid
    out["quarter_verdict"] = ("invalid" if not valid else
                              "the closing quarter differs" if len(q_hits) >= 2 and q_same else
                              "no difference at this resolution")
    out["data_verdict"] = ("invalid" if not valid else
                           "the data start differs" if len(d_hits) == len(DATA) and d_same else
                           "no difference at this resolution")
    out["others"] = len(out["others"])
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    sys.exit(main())
