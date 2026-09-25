#!/usr/bin/env python3
"""
What our receiver makes of the packets it loses, read with an implicit header, and the PHY header
fields of the E22's packets. Rules in docs/PHY-notes.md (2026-09-25), declared before the run.

The T-Deck alternates, in blocks, between the normal explicit header (`IMPL 0`) and an implicit
header of 51 bytes with CRC off (`IMPL 51`), which hands over every packet it synchronises on with
the transmitter's header nibbles as its first bytes. A frame is given to the packet whose
transmission started last before it, within 2 s.

In implicit blocks every frame is compared with the E22's most common frame, at the positions where
at least 90 % of the E22's frames agree (the rest follow the sequence number, and the E22 is taken
as clean). A frame is clean if every such position agrees; otherwise its differing positions say
where it broke.

  bench/analyze_impl.py run.json [--rx tdeck] [--arm heltec] [--ref e22]
  bench/analyze_impl.py --self-check
"""

import argparse
import collections
import json
import re
import statistics as st
import sys

QUALITY = re.compile(r"rx quality rssi (-?\d+) dBm, snr (-?\d+\.\d+) dB")
REGS = re.compile(r"rx header regs 0749=([0-9A-F]{2}) 076B=([0-9A-F]{2})")
RAW = re.compile(r"rx raw: ([0-9A-F]+)")
FREQ = re.compile(r"sx1262: freq error ([+-]?\d+) Hz")
FAIL = re.compile(r"rx fail (hdr|crc) ")
FIRST_BLOCK = 5          # the first 8 symbols carry 9 nibbles: output bytes 0-4


def records(lines, times):
    """One dict per reception: t, raw, rssi, snr, regs, fe; header errors as {'t', 'fail'}."""
    out, cur = [], {}
    for line, t in zip(lines, times):
        m = QUALITY.search(line)
        if m:
            cur = {"rssi": int(m[1]), "snr": float(m[2])}
            continue
        m = REGS.search(line)
        if m and cur:
            cur["regs"] = (int(m[1], 16), int(m[2], 16))
            continue
        m = RAW.search(line)
        if m and cur:
            cur["raw"], cur["t"] = bytes.fromhex(m[1]), t
            out.append(cur)
            continue
        m = FREQ.search(line)
        if m and out and out[-1] is cur:
            cur["fe"] = int(m[1])
            cur = {}
            continue
        if FAIL.search(line):
            out.append({"t": t, "fail": True})
    return out


def attribute(recs, starts, window=2.0):
    order = sorted(starts.items(), key=lambda kv: kv[1])
    for r in recs:
        seq = None
        for s, t0 in order:
            if t0 > r["t"]:
                break
            seq = s if r["t"] - t0 <= window else None
        r["seq"] = seq
    return recs


def reference(frames, agree=0.9):
    """(mode per position, positions where at least `agree` of the frames hold the mode)."""
    n = min(len(f) for f in frames)
    mode, fixed = [], []
    for i in range(n):
        value, count = collections.Counter(f[i] for f in frames).most_common(1)[0]
        mode.append(value)
        if count >= agree * len(frames):
            fixed.append(i)
    return mode, fixed


def classify(frame, mode, fixed):
    bad = [i for i in fixed if frame[i] != mode[i]]
    if not bad:
        return "clean", bad
    if len(bad) > len(fixed) / 2:
        return "whole frame", bad
    if max(bad) < FIRST_BLOCK:
        return "first block only", bad
    return "past the first block", bad


def analyse(run, rx, arm, ref):
    sent = {int(k): v for k, v in run["sent"].items()}
    state = {int(k): v for k, v in run["neighbour"].items()}
    seen = {int(x) for x in run["seen"][rx]}
    starts = {int(k): v[0] for k, v in run["tx_times"].items()}
    recs = attribute(records(run["log"][rx], run["log_t"][rx]), starts)
    by_seq = collections.defaultdict(list)
    for r in recs:
        if r["seq"] is not None:
            by_seq[r["seq"]].append(r)
    out = {"unattributed": sum(r["seq"] is None for r in recs)}

    # Explicit blocks: delivery, header errors, and the header fields as registers.
    expl = {}
    for w in (arm, ref):
        seqs = [s for s in sent if sent[s] == w and state.get(s) == "IMPL 0"]
        regs = [r["regs"] for s in seqs for r in by_seq[s] if "regs" in r and "raw" in r]
        cr = collections.Counter((a >> 4) & 7 for a, _ in regs)
        crc = collections.Counter((b >> 4) & 1 for _, b in regs)
        expl[w] = {"delivered": f"{sum(s in seen for s in seqs)}/{len(seqs)}",
                   "header_errors": sum(any(r.get("fail") for r in by_seq[s]) for s in seqs),
                   "cr_field": dict(cr), "crc_flag": dict(crc)}
    out["explicit"] = expl

    # Implicit blocks: every frame, clean or not, and where it broke.
    impl, frames = {}, {}
    for w in (arm, ref):
        seqs = [s for s in sent if sent[s] == w and state.get(s) == "IMPL 51"]
        got = {s: next((r for r in by_seq[s] if "raw" in r and len(r["raw"]) == 51), None) for s in seqs}
        frames[w] = {s: r for s, r in got.items() if r}
        impl[w] = {"packets": len(seqs), "frames": len(frames[w])}
    mode, fixed = reference([r["raw"] for r in frames[ref].values()]) if frames[ref] else ([], [])
    out["reference"] = {"mode": bytes(mode).hex(), "fixed_positions": len(fixed),
                        "fixed_in_first_block": sum(i < FIRST_BLOCK for i in fixed)}
    for w in (arm, ref):
        if not frames[w] or not fixed:
            continue
        kinds = collections.Counter()
        detail = {"clean": [], "damaged": []}
        for s, r in sorted(frames[w].items()):
            kind, bad = classify(r["raw"], mode, fixed)
            kinds[kind] += 1
            r["kind"], r["bad"] = kind, bad
            detail["clean" if kind == "clean" else "damaged"].append(r)
        impl[w]["kinds"] = dict(kinds)
        for k, rs in detail.items():
            if rs:
                impl[w][k] = {"n": len(rs),
                              "rssi_median": st.median(x["rssi"] for x in rs),
                              "snr_median": st.median(x["snr"] for x in rs),
                              "fe_median": st.median(x["fe"] for x in rs if "fe" in x) if any("fe" in x for x in rs) else None}
        impl[w]["damaged_frames"] = [{"seq": s, "kind": r["kind"], "bad": r["bad"][:12], "rssi": r["rssi"],
                                      "snr": r["snr"], "fe": r.get("fe"), "raw": r["raw"].hex()}
                                     for s, r in sorted(frames[w].items()) if r["kind"] != "clean"]
    out["implicit"] = impl

    # Decision, as declared.
    e = impl.get(ref, {})
    ref_ok = (e.get("packets") and e["frames"] >= 0.9 * e["packets"]
              and e.get("kinds", {}).get("clean", 0) >= 0.95 * e["frames"])
    own = expl[arm]
    q1_ok = sum(own["cr_field"].values()) and own["cr_field"].get(1, 0) >= 0.95 * sum(own["cr_field"].values()) \
        and own["crc_flag"].get(1, 0) >= 0.95 * sum(own["crc_flag"].values())
    out["valid"] = bool(ref_ok)
    theirs = expl[ref]
    out["q1"] = ("registers not understood" if not q1_ok else
                 "same CR and CRC flag" if theirs["cr_field"].get(1, 0) >= 0.95 * max(1, sum(theirs["cr_field"].values()))
                 and theirs["crc_flag"].get(1, 0) >= 0.95 * max(1, sum(theirs["crc_flag"].values()))
                 else "different")
    h = impl[arm]
    lost = h["packets"] - h.get("kinds", {}).get("clean", 0)
    damaged = h["frames"] - h.get("kinds", {}).get("clean", 0)
    k = h.get("kinds", {})
    out["q2"] = ("invalid" if not ref_ok else
                 "the loss disappears without the header check" if lost <= 0.03 * h["packets"] else
                 "not detected at all" if h["packets"] - h["frames"] >= 0.5 * lost else
                 "damage confined to the first block" if damaged and k.get("first block only", 0) >= 0.8 * damaged else
                 "whole-frame misdecode" if damaged and k.get("whole frame", 0) >= 0.5 * damaged else
                 "mixed")
    return out


def self_check():
    lines = ["sx1262: rx quality rssi -53 dBm, snr 7.50 dB, signal rssi -54 dBm",
             "sx1262: rx header regs 0749=17 076B=3F",
             "sx1262: rx frame 51 bytes, header 1D ED",
             "sx1262: rx raw: 1DED",
             "sx1262: freq error -13 Hz (raw 28)",
             "sx1262: rx fail hdr rssi -50 dBm, snr 7.00 dB, signal rssi -50 dBm, preamble->fail 70 ms"]
    recs = records(lines, [1.0, 1.0, 1.0, 1.1, 1.1, 3.0])
    assert recs[0] == {"rssi": -53, "snr": 7.5, "regs": (0x17, 0x3F), "raw": b"\x1d\xed", "t": 1.1, "fe": -13}
    assert recs[1] == {"t": 3.0, "fail": True}
    attribute(recs, {1: 0.5, 2: 2.5})
    assert [r["seq"] for r in recs] == [1, 2]
    mode, fixed = reference([b"\x01\x02\x03", b"\x01\x02\x04", b"\x01\x02\x05", b"\x01\x02\x06", b"\x01\x09\x03"], 0.8)
    assert mode[:2] == [1, 2] and fixed == [0, 1]
    assert classify(b"\x01\x02\x07", mode, fixed)[0] == "clean"
    assert classify(b"\x01\x05\x07", mode, fixed)[0] == "first block only"
    assert classify(bytes(range(20)), list(range(20)), list(range(20)))[0] == "clean"
    assert classify(bytes([0] * 20), list(range(1, 21)), list(range(20)))[0] == "whole frame"
    far = bytes(range(10)) + bytes([99]) + bytes(range(11, 20))
    assert classify(far, list(range(20)), list(range(20)))[0] == "past the first block"
    print("analyze_impl: self-check passes")


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
