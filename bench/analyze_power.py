#!/usr/bin/env python3
"""
What leaves a transmitter's antenna at each power level, read against a reference transmitter
whose levels are known, from interleave.py runs. Rules in docs/PHY-notes.md (2026-10-08),
declared before the run.

  bench/analyze_power.py run1.json [run2.json ...] [--dut h] [--ref e] [--ref-dbm 22,17,13,10]
  bench/analyze_power.py --self-check

Arms are named by prefix and level: h0..h3 for the transmitter measured, e0..e3 for the
reference. Every receiver that logged the packets is used; one that does not hold the
reference's own steps is left out of that run.
"""

import argparse
import json
import re
import statistics
import sys

# "bridge: rx 38 bytes, rssi -58: 00000042AAAA..." - the sequence number leads the payload.
RX = re.compile(r"rx \d+ bytes, rssi (-?\d+): (\d{8})")
MIN_PACKETS = 20    # fewer decoded than this and an arm's median says too little
SATURATED = -30     # at or above this these receivers saturate; 0 means no reading at all
LADDER_TOL = 3.0    # how far the reference's own steps may stray from its datasheet
SPREAD_FLOOR = 2.0  # never claim better than this: the RSSI scale and the datasheet both


def medians(run):
    """{receiver: {arm: median RSSI}}, valid arms only, and the reasons the others were not."""
    sent = {int(k): v for k, v in run["sent"].items()}
    out, dropped = {}, []
    for rx, lines in run["log"].items():
        by_arm = {}
        for line in lines:
            m = RX.search(line)
            if m and int(m.group(2)) in sent:
                by_arm.setdefault(sent[int(m.group(2))], {})[int(m.group(2))] = int(m.group(1))
        out[rx] = {}
        for arm, readings in sorted(by_arm.items()):
            values = list(readings.values())
            if len(values) < MIN_PACKETS:
                dropped.append(f"{rx}/{arm}: {len(values)} decoded")
            elif any(v == 0 or v >= SATURATED for v in values):
                dropped.append(f"{rx}/{arm}: saturated")
            else:
                out[rx][arm] = statistics.median(values)
    return out, dropped


def estimate(arms, dut, ref, ref_dbm):
    """{dut level: dBm}, each against the reference level nearest it in received strength, so
    the receiver's scale only has to hold over a few dB. None if the reference's steps fail."""
    refs = {int(a[len(ref):]): v for a, v in arms.items() if a.startswith(ref)}
    if 0 not in refs:
        return None
    for level, v in refs.items():
        if abs((v - refs[0]) - (ref_dbm[level] - ref_dbm[0])) > LADDER_TOL:
            return None
    result = {}
    for a, v in arms.items():
        if a.startswith(dut):
            near = min(refs, key=lambda level: abs(refs[level] - v))
            result[int(a[len(dut):])] = ref_dbm[near] + (v - refs[near])
    return result


def combine(estimates):
    """Mean and spread per level over every (run, receiver) that produced one."""
    levels = sorted({level for e in estimates for level in e})
    out = {}
    for level in levels:
        values = [e[level] for e in estimates if level in e]
        spread = max(SPREAD_FLOOR, (max(values) - min(values)) / 2)
        out[level] = (statistics.mean(values), spread, len(values))
    return out


def report(runs, dut, ref, ref_dbm):
    estimates = []
    for name, run in runs:
        meds, dropped = medians(run)
        print(f"\n{name}")
        for d in dropped:
            print(f"  dropped {d}")
        for rx, arms in meds.items():
            row = "  ".join(f"{a} {v:6.1f}" for a, v in sorted(arms.items()))
            e = estimate(arms, dut, ref, ref_dbm)
            print(f"  {rx:>8}: {row}")
            if e is None:
                print(f"  {rx:>8}: reference steps off its datasheet - left out")
            else:
                estimates.append(e)
                print(f"  {rx:>8}: " + "  ".join(f"{dut}{k} {v:5.1f} dBm" for k, v in sorted(e.items())))
    if not estimates:
        print("\nno receiver held the reference: no result")
        return None
    result = combine(estimates)
    print("\nresult, mean over receivers and runs")
    for level, (mean, spread, n) in result.items():
        print(f"  level {level}: {mean:5.1f} dBm +/- {spread:.1f}  ({n} readings)")
    if 0 in result:
        top = result[0][0]
        print("  amplifier " + ("in use" if top >= 20 else "bypassed" if top < 13 else "unclear")
              + f": level 0 at {top:.1f} dBm")
    if all(0 in e and 1 in e for e in estimates):
        steps = [e[0] - e[1] for e in estimates]
        verdict = "compressed at the top" if all(s < 2 for s in steps) else "not compressed at the top"
        print(f"  {verdict}: level 1 to 0 adds {min(steps):.1f} to {max(steps):.1f} dB (4 dB at the chip)")
    return result


def self_check():
    sent, logs = {}, {"near": [], "far": []}
    levels = {"e0": -60, "e1": -65, "e2": -69, "e3": -72, "h0": -55, "h1": -58, "h2": -60, "h3": -62}
    seq = 0
    for _ in range(25):
        for arm, rssi in levels.items():
            seq += 1
            sent[str(seq)] = arm
            logs["near"].append(f"I (1) bridge: rx 38 bytes, rssi {rssi}: {seq:08d}AAAA...")
            logs["far"].append(f"I (1) bridge: rx 38 bytes, rssi {rssi - 20}: {seq:08d}AAAA...")
    meds, dropped = medians({"sent": sent, "log": logs})
    assert dropped == []
    e = estimate(meds["near"], "h", "e", [22, 17, 13, 10])
    # h3 at -62 is 2 dB from e0 and 3 from e1, so it is read against e0.
    assert e == {0: 27, 1: 24, 2: 22, 3: 20}, e
    assert estimate(meds["far"], "h", "e", [22, 17, 13, 10]) == e, "a common offset must cancel"
    # A reference whose steps are off its datasheet is not used.
    assert estimate(dict(meds["near"], e3=-60), "h", "e", [22, 17, 13, 10]) is None
    # Too few packets, or a saturated reading, drops the arm at that receiver.
    few = {"sent": {"1": "h0"}, "log": {"near": ["rx 38 bytes, rssi -50: 00000001..."]}}
    assert medians(few)[1] == ["near/h0: 1 decoded"]
    assert combine([{0: 26}, {0: 27}])[0] == (26.5, 2.0, 2)
    print("analyze_power: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="*")
    ap.add_argument("--dut", default="h", help="arm prefix of the transmitter measured")
    ap.add_argument("--ref", default="e", help="arm prefix of the reference")
    ap.add_argument("--ref-dbm", default="22,17,13,10", help="the reference's dBm at levels 0-3")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        self_check()
        return 0
    if not args.runs:
        ap.error("give one or more interleave.py --out files")
    ref_dbm = [float(x) for x in args.ref_dbm.split(",")]
    runs = [(path, json.load(open(path))) for path in args.runs]
    return 0 if report(runs, args.dut, args.ref, ref_dbm) else 1


if __name__ == "__main__":
    sys.exit(main())
