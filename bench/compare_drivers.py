#!/usr/bin/env python3
"""Interleave function substitutions on one Heltec with an unchanged E22 control."""
import argparse
import json
import re
import random
import time
from collections import Counter
from pathlib import Path

from interleave import Receiver, Transmitter, build_schedule


def send(port, seq, extra, replies):
    port.port.write(f'TX seq={seq} {extra}\n'.encode())
    port.port.flush()
    end, buf = time.monotonic() + 8, ''
    while time.monotonic() < end:
        buf += port.port.read(4096).decode('utf-8', 'replace')
        match = next((m for m in re.finditer(r'\bTX(OK|FAIL) seq=(\d+)\b', buf)
                      if int(m[2]) == seq), None)
        if match:
            replies[seq] = buf
            return match[1] == 'OK'
        if 'TXERR' in buf:
            break
    replies[seq] = buf
    raise RuntimeError(f'{port.name}: no matching reply to {seq}: {buf}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--n', type=int, default=24)
    ap.add_argument('--modes', default='4,1,2,5')
    ap.add_argument('--gap', type=float, default=0.8)
    ap.add_argument('--extra', default='')
    ap.add_argument('--seed', type=int, default=20260921)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    modes = args.modes.split(',')
    if args.n <= 0 or not all(m in tuple(map(str, range(9))) for m in modes) or len(set(modes)) != len(modes):
        ap.error('n must be positive; modes must be unique numbers from 0 to 8')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    tx = Transmitter('heltec', '/dev/ttyACM1')
    control = Transmitter('e22', '/dev/ttyUSB1')
    receivers = [Receiver('tdeck', 'morlov@10.10.10.190:/dev/ttyACM0'),
                 Receiver('e220', 'morlov@10.10.10.190:/dev/ttyUSB0')]
    sent, attempted, replies = {}, {}, {}
    try:
        for rx in receivers:
            rx.start()
        time.sleep(6)
        for rx in receivers:
            boot = [l for l in rx.lines if 'bridge up' in l]
            if not boot or 'online=1' not in boot[-1]:
                raise RuntimeError(f'{rx.name}: no live receiver boot banner')
            print(rx.name, boot[-1], flush=True)
        # Randomize each palindromic block so a periodic interferer cannot always hit one mode.
        rng, schedule = random.Random(args.seed), []
        for start in range(0, args.n, 2):
            order = modes + ['e22']
            rng.shuffle(order)
            schedule += build_schedule(order, min(2, args.n - start))
        for seq, variant in enumerate(schedule, 1):
            port = control if variant == 'e22' else tx
            if variant != 'e22':
                port.port.write(f'MODE {variant}\n'.encode())
                port.port.flush()
            attempted[seq] = {'variant': variant, 'time': time.time()}
            if send(port, seq, args.extra, replies):
                sent[seq] = variant
            else:
                raise RuntimeError(f'TX failed: {replies[seq]}')
            if seq % len(modes + ['e22']) == 0:
                counts = Counter(sent.values())
                print(f'{seq}/{len(schedule)}', {
                    name: (counts[name], {r.name: sum(s in r.seen for s, v in sent.items() if v == name)
                                          for r in receivers})
                    for name in modes + ['e22']}, flush=True)
            time.sleep(args.gap)
        print('waiting for final cumulative counters', flush=True)
        time.sleep(35)
    finally:
        for rx in receivers:
            rx.close()
        tx.port.close()
        control.port.close()
        args.out.write_text(json.dumps({'attempted': attempted, 'sent': sent, 'replies': replies,
            'seen': {r.name: r.seen for r in receivers},
            'log': {r.name: r.lines for r in receivers}, 'args': vars(args) | {'out': str(args.out)}}, indent=1))
    for rx in receivers:
        boot = [l for l in rx.lines if 'bridge up' in l]
        stages = [l for l in rx.lines if 'stages:' in l]
        print(rx.name, boot[-1:] or ['INVALID: no boot banner'])
        print(rx.name, stages[-1:])
    for name in modes + ['e22']:
        seqs = {s for s, v in sent.items() if v == name}
        print(name, 'sent', len(seqs), {r.name: len(seqs & r.seen.keys()) for r in receivers})


if __name__ == '__main__':
    main()
