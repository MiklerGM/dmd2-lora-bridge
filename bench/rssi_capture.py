#!/usr/bin/env python3
"""Capture narrowband RSSI on T-Deck while E220 judges interleaved Heltec/E22 packets."""
import argparse
import gzip
import json
import random
import struct
import time
from pathlib import Path

from interleave import Node, build_schedule


def scope_reply(scope, mark, kind, seq=None, timeout=8, incomplete_ok=False):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with scope.lock:
            fresh = scope.lines[mark:]
        for line in fresh:
            mark += 1
            if not line.startswith('{'):
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                if incomplete_ok:
                    continue  # DUMP terminates the previous interrupted USB record.
                raise
            if record.get('type') == 'error':
                raise RuntimeError(f'scope: {record}')
            if record.get('type') == kind and (seq is None or record.get('seq') == seq):
                return record, mark
        time.sleep(0.005)
    raise TimeoutError(f'no scope {kind} for seq={seq}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--n', type=int, default=8, help='packets per transmitter')
    ap.add_argument('--offset', type=int, nargs='+', default=[0],
                    help='scope RX offsets, kHz, within +/-240; interleaved when several are given')
    ap.add_argument('--heltec', default='/dev/ttyACM0')
    ap.add_argument('--e22', default='/dev/ttyUSB1')
    ap.add_argument('--e220', default='morlov@10.10.10.190:/dev/ttyUSB1')
    ap.add_argument('--scope', default='morlov@10.10.10.190:/dev/ttyACM0')
    ap.add_argument('--seed', type=int, default=20260922)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    if args.n <= 0:
        ap.error('n must be positive')
    if any(abs(o) > 240 for o in args.offset):
        ap.error('offsets must be within +/-240 kHz')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Ports move when boards are replugged; identify them by boot banner before a run.
    specs = {'heltec': args.heltec, 'e22': args.e22, 'e220': args.e220, 'scope': args.scope}
    nodes, attempts = {}, []
    error = None
    try:
        for name, spec in specs.items():
            nodes[name] = Node(name, spec)
        time.sleep(6)
        for name in ('heltec', 'e22', 'e220'):
            boot = [s for s in nodes[name].lines if 'bridge up' in s]
            if not boot or 'online=1' not in boot[-1]:
                raise RuntimeError(f'{name}: missing healthy boot banner')
            print(name, boot[-1], flush=True)
        scope = nodes['scope']
        ready, _ = scope_reply(scope, 0, 'ready')
        if ready.get('status') != 0:
            raise RuntimeError(f'scope initialization failed: {ready}')
        print('scope', ready, flush=True)
        offsets = list(dict.fromkeys(args.offset))
        rng = random.Random(args.seed)
        schedule = [(None, offset) for offset in offsets for _ in range(2)]
        for start in range(0, args.n, 2):
            rng.shuffle(offsets)
            for offset in offsets:
                order = ['heltec', 'e22']
                rng.shuffle(order)
                schedule += [(tx, offset) for tx in build_schedule(order, min(2, args.n - start))]
        for index, (tx, offset) in enumerate(schedule):
            seq = 100001 + index
            with scope.lock:
                mark = len(scope.lines)
            attempt = {'seq': seq, 'tx': tx, 'offset_khz': offset, 'command_time': time.time()}
            attempts.append(attempt)
            scope.write(f'CAP seq={seq} offset={offset}\n')
            armed, mark = scope_reply(scope, mark, 'armed', seq)
            attempt['armed'] = armed
            attempt['armed_host_time'] = time.time()
            time.sleep(0.05)
            if tx:
                attempt['tx_command_time'] = time.time()
                if not nodes[tx].send(seq, ''):
                    raise RuntimeError(f'{tx}: transmission {seq} not confirmed')
                attempt['ack_host_time'] = time.time()
            for retry in range(3):
                try:
                    capture, _ = scope_reply(scope, mark, 'capture', seq, timeout=12,
                                              incomplete_ok=bool(retry))
                    if capture['offset_khz'] != offset:
                        raise RuntimeError('scope reported a different frequency offset')
                    if capture.get('status') != 0:
                        raise RuntimeError(f'scope read failed: status={capture.get("status")}')
                    rssi = bytes.fromhex(capture['rssi_hex'])
                    raw_times = bytes.fromhex(capture['t_us_le32_hex'])
                    if len(rssi) != capture['n_samples'] or not rssi or len(raw_times) != 4 * len(rssi):
                        raise ValueError('scope sample/timestamp lengths differ')
                    times = struct.unpack(f'<{len(rssi)}I', raw_times)
                    if any(b <= a for a, b in zip(times, times[1:])):
                        raise ValueError('scope timestamps are not increasing')
                    break
                except (TimeoutError, ValueError, KeyError) as exc:
                    attempt.setdefault('usb_retries', []).append(repr(exc))
                    if retry == 2:
                        raise
                    print(f'seq={seq}: repeat USB download of the same capture: {exc}', flush=True)
                    with scope.lock:
                        mark = len(scope.lines)
                    scope.write(f'DUMP seq={seq}\n')
            attempt['capture'] = capture
            attempt['capture_host_time'] = time.time()
            print(f'{index + 1}/{len(schedule)} seq={seq} tx={tx or "quiet"} offset={offset} '
                  f'n={len(rssi)} span={times[-1] - times[0]}us '
                  f'e220={seq in nodes["e220"].seen}', flush=True)
            time.sleep(0.8)
        time.sleep(2)
    except BaseException as exc:
        error = repr(exc)
        raise
    finally:
        for node in nodes.values():
            node.close()
        receiver = nodes.get('e220')
        for attempt in attempts:
            attempt['e220_received'] = attempt['seq'] in receiver.seen if receiver else None
        result = {
            'args': vars(args) | {'out': str(args.out)}, 'specs': specs,
            'attempts': attempts, 'error': error,
            'log': {name: node.lines for name, node in nodes.items()},
            'seen': {name: node.seen for name, node in nodes.items()},
            'qualification': 'Before waveform inference: preamble peak period 4096 +/- 100 us in both sources; retain real sampling times.',
        }
        opener = gzip.open if args.out.suffix == '.gz' else open
        with opener(args.out, 'wt') as out:
            json.dump(result, out)
    for tx in ('heltec', 'e22'):
        arm = [a for a in attempts if a['tx'] == tx]
        print(tx, sum(a['e220_received'] for a in arm), '/', len(arm), flush=True)


if __name__ == '__main__':
    main()
