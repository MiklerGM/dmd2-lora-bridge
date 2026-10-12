#!/usr/bin/env python3
"""
Each transmitter's own power steps, read off an RTL-SDR recording made during an interleave.py
run. Rules in docs/PHY-notes.md (2026-10-08, run 2), declared before the run.

  bench/analyze_sdr_power.py run.json capture.cu8 --start EPOCH [--fs 1024000]
  bench/analyze_sdr_power.py --self-check

EPOCH is time.time() taken just before rtl_sdr started. Bursts come from sdr_lora.py; each is
given to the packet whose transmit window it falls in, once the recording's clock is lined up
with the run's by the offset that places the most bursts. A transmitter's packets share a
preamble length, so a burst with another one (the phone's bridge, say) is not taken for ours.
Levels from a dongle without an antenna depend on where each board stands, so only steps within
one transmitter are reported, never one transmitter against another.
"""

import argparse
import collections
import contextlib
import io
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

BURST = re.compile(r"packet \d+ at\s+([\d.]+) s: level ([+-][\d.]+) dBFS \(clipped ([\d.]+)%\), "
                   r"carrier\s+([+-][\d.]+) kHz, preamble (\d+) symbols")
MIN_BURSTS = 20
LADDER_TOL = 3.0


def read_bursts(capture, fs):
    """[(seconds into the recording, dBFS, clipped fraction, preamble symbols)] via sdr_lora.py."""
    out = subprocess.run([sys.executable, str(Path(__file__).with_name("sdr_lora.py")), str(capture),
                          "--fs", str(fs)], capture_output=True, text=True, check=True).stdout
    return [(float(m[1]), float(m[2]), float(m[3]) / 100, int(m[5])) for m in BURST.finditer(out)]


def match(bursts, windows, start):
    """{seq: burst} for the clock offset that matches the most bursts. windows: {seq: (t0, t1)}."""
    best = {}
    for step in range(-300, 301):              # +/-3 s in 10 ms: rtl_sdr takes a moment to start
        offset = step / 100
        got, used = {}, set()
        for i, b in enumerate(bursts):
            t = start + offset + b[0]
            for seq, (t0, t1) in windows.items():
                if seq not in got and i not in used and t0 <= t <= t1 + 0.3:
                    got[seq] = b
                    used.add(i)
                    break
        if len(got) > len(best):
            best = got
    return best


def ladders(matched, sent):
    """{transmitter prefix: {level: (median dBFS, n)}}, unclipped bursts with the usual preamble."""
    by_arm = collections.defaultdict(list)
    for seq, b in matched.items():
        by_arm[sent[seq]].append(b)
    pre = {}
    for arm, bs in by_arm.items():
        pre.setdefault(arm[0], collections.Counter()).update(b[3] for b in bs)
    out = collections.defaultdict(dict)
    for arm, bs in sorted(by_arm.items()):
        usual = pre[arm[0]].most_common(1)[0][0]
        # Within a symbol: the E22's reads 19 or 20 at random, while other parts are far off it
        # (the Heltec 8, an E220 30).
        levels = [b[1] for b in bs if b[2] == 0 and abs(b[3] - usual) <= 1]
        out[arm[0]][int(arm[1:])] = (statistics.median(levels) if levels else None, len(levels))
    return out


def ladder_holds(steps, ref_dbm):
    """Whether a transmitter's steps from level 0 are within LADDER_TOL of its datasheet's. Weak
    levels may be missing, but level 0 alone is no step at all and holds nothing."""
    return any(k != 0 for k in steps) and all(
        abs(steps[k] - (ref_dbm[k] - ref_dbm[0])) <= LADDER_TOL for k in steps)


def report(run, bursts, start, ref, ref_dbm):
    sent = {int(k): v for k, v in run["sent"].items()}
    windows = {int(k): tuple(v) for k, v in run["tx_times"].items() if int(k) in sent}
    matched = match(bursts, windows, start)
    print(f"{len(bursts)} bursts in the recording, {len(matched)} matched to {len(windows)} packets")
    holds = None
    for tx, levels in sorted(ladders(matched, sent).items()):
        if 0 not in levels or levels[0][0] is None or levels[0][1] < MIN_BURSTS:
            print(f"  {tx}: level 0 has too few clean bursts")
            continue
        steps = {k: v[0] - levels[0][0] for k, v in levels.items() if v[0] is not None and v[1] >= MIN_BURSTS}
        print(f"  {tx}: " + "  ".join(
            f"level {k} " + (f"{v:+6.1f} dBFS" if v is not None else "excluded, no clean burst") + f" ({n})"
            for k, (v, n) in sorted(levels.items())))
        print(f"  {tx}: steps from level 0 " + "  ".join(f"{k}: {steps[k]:+5.1f}" for k in sorted(steps) if k))
        if tx == ref:
            holds = ladder_holds(steps, ref_dbm)
            print(f"  {tx}: the reference's steps " + ("hold" if holds else "do NOT hold")
                  + f" against its datasheet ({', '.join(f'{d - ref_dbm[0]:+g}' for d in ref_dbm[1:])})")
    return holds


def self_check():
    windows = {1: (100.0, 100.6), 2: (102.0, 102.5), 3: (104.0, 104.7)}
    # The recording started 2.5 s before EPOCH says, and a stray burst sits between packets.
    bursts = [(102.8, -20.0, 0.0, 8), (103.6, -40.0, 0.0, 30), (104.7, -25.0, 0.0, 19), (106.9, -26.0, 0.0, 8)]
    got = match(bursts, windows, start=0 - 2.5)
    assert sorted(got) == [1, 2, 3] and got[2][1] == -25.0, got
    sent = {1: "h0", 2: "e0", 3: "h1"}
    lad = ladders(got, sent)
    assert lad["h"] == {0: (-20.0, 1), 1: (-26.0, 1)} and lad["e"] == {0: (-25.0, 1)}, lad
    # A preamble read a symbol long still counts; one from another kind of part does not.
    mixed = {1: (0, -20.0, 0.0, 19), 2: (0, -22.0, 0.0, 20), 3: (0, -21.0, 0.0, 19), 4: (0, -40.0, 0.0, 30)}
    assert ladders(mixed, {1: "e0", 2: "e0", 3: "e0", 4: "e0"})["e"][0] == (-21.0, 3)
    assert ladder_holds({0: 0.0, 1: -4.0, 2: -10.5, 3: -11.0}, [22, 17, 13, 10])
    assert not ladder_holds({0: 0.0, 1: -2.0, 2: -2.0, 3: -5.0}, [22, 17, 13, 10])
    # A clean level 0 beside a level whose every burst clipped: reported rather than a traceback,
    # and with no step left to check, the reference is not called good.
    run = {"sent": {s: "e0" if s <= 20 else "e1" for s in range(1, 22)},
           "tx_times": {s: (10.0 * s, 10.0 * s + 0.5) for s in range(1, 22)}}
    bursts = [(10.0 * s + 0.1, -20.0 if s <= 20 else -5.0, 0.0 if s <= 20 else 0.4, 8) for s in range(1, 22)]
    with contextlib.redirect_stdout(io.StringIO()):
        assert report(run, bursts, 0.0, "e", [22, 17, 13, 10]) is False
    print("analyze_sdr_power: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("capture", nargs="?")
    ap.add_argument("--start", type=float, help="time.time() just before rtl_sdr started")
    ap.add_argument("--fs", type=float, default=1024000)
    ap.add_argument("--ref", default="e", help="arm prefix of the reference transmitter")
    ap.add_argument("--ref-dbm", default="22,17,13,10")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        self_check()
        return 0
    if not (args.run and args.capture and args.start):
        ap.error("give run.json, capture.cu8 and --start")
    ref_dbm = [float(x) for x in args.ref_dbm.split(",")]
    run = json.load(open(args.run))
    if run.get("invalid"):   # set by interleave.py; captures from before it have none
        print("marked invalid, not used: " + "; ".join(run["invalid"]))
        return 1
    holds = report(run, read_bursts(args.capture, args.fs), args.start, args.ref, ref_dbm)
    return 0 if holds else 1


if __name__ == "__main__":
    sys.exit(main())
