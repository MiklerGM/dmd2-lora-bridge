#!/usr/bin/env python3
"""Qualify RSSI pulse timing against the criteria fixed before the capture.

Requires NumPy; matplotlib is needed only with --plot. No hardware access.

  python3 bench/analyze_rssi.py capture.json.gz --out /tmp/rssi-center
  python3 bench/analyze_rssi.py center.json.gz offsets.json.gz --out /tmp/rssi --plot

Writes PREFIX.analysis.json and, optionally, PREFIX.png. The JSON retains every
pulse centroid, the first qualifying six-pulse chain, and its fitted period.
Qualification is per frequency offset and transmitter: at least eight captures,
at least 75% qualifying chains (6/8), and at least two quiet captures with no chains.
This qualifies the timing measurement, not RF interoperability or payload decoding.
"""

import argparse
import gzip
import json
from collections import defaultdict
from pathlib import Path

import numpy as np


def load_captures(paths):
    captures = []
    for path in paths:
        opener = gzip.open if path.suffix == '.gz' else open
        with opener(path, 'rt') as stream:
            data = json.load(stream)
        if data.get('error'):
            raise ValueError(f'{path}: capture run failed: {data["error"]}')
        for attempt in data['attempts']:
            capture = attempt['capture']
            raw_rssi = bytes.fromhex(capture['rssi_hex'])
            raw_times = bytes.fromhex(capture['t_us_le32_hex'])
            if (capture['status'] != 0 or capture['seq'] != attempt['seq']
                    or not raw_rssi or len(raw_rssi) != capture['n_samples']
                    or len(raw_times) != 4 * len(raw_rssi)):
                raise ValueError(f'{path}, seq={attempt["seq"]}: invalid capture or raw lengths')
            t = np.frombuffer(raw_times, dtype='<u4').astype(float)
            y = -np.frombuffer(raw_rssi, dtype=np.uint8).astype(float) / 2
            if np.any(np.diff(t) <= 0):
                raise ValueError(f'{path}, seq={attempt["seq"]}: timestamps are not increasing')
            offset = capture['offset_khz']
            if attempt.get('offset_khz', offset) != offset:
                raise ValueError(f'{path}, seq={attempt["seq"]}: frequency offsets differ')
            captures.append((str(path), attempt, t, y))
    if not captures:
        raise ValueError('no captures')
    return captures


def analyze(captures):
    by_offset = defaultdict(list)
    for item in captures:
        by_offset[item[1]['capture']['offset_khz']].append(item)

    rows, offsets = [], []
    for offset, group in sorted(by_offset.items()):
        quiet = [y for _, a, _, y in group if a['tx'] is None]
        if not quiet:
            raise ValueError(f'offset={offset}: no quiet capture for threshold')
        percentiles = np.percentile(np.concatenate(quiet), [0, 50, 99, 100])
        threshold = float(percentiles[2] + 10)
        offset_rows = []
        for path, attempt, t, y in group:
            active = np.flatnonzero(y > threshold)
            pulses = (np.split(active, np.flatnonzero(np.diff(t[active]) > 250) + 1)
                      if len(active) else [])
            peaks = []
            for pulse in pulses:
                if len(pulse) < 2:
                    continue
                weights = 10 ** ((y[pulse] - threshold) / 10) - 1
                peaks.append(float(np.sum(t[pulse] * weights) / np.sum(weights)))
            peaks = np.array(peaks)
            train, period, residual = [], None, None
            for begin in range(len(peaks) - 5):
                candidate = peaks[begin:begin + 6]
                if candidate[0] - peaks[0] > 20000:
                    break
                slope, origin = np.polyfit(np.arange(6), candidate, 1)
                error = float(np.max(np.abs(candidate - (origin + slope * np.arange(6)))))
                intervals = np.diff(candidate)
                sample_times = t[(t >= candidate[0]) & (t <= candidate[-1])]
                if (abs(slope - 4096) <= 100 and error <= 150
                        and np.all((intervals >= 3800) & (intervals <= 4400))
                        and len(sample_times) >= 2 and np.max(np.diff(sample_times)) <= 250):
                    train, period, residual = candidate.tolist(), float(slope), error
                    break
            capture = attempt['capture']
            row = {
                'source': path, 'seq': attempt['seq'], 'tx': attempt['tx'],
                'offset_khz': offset, 'received': attempt['e220_received'],
                'n': len(t), 'skipped': capture['skipped_slots'],
                'dt_max': capture['dt_max_us'],
                'raw_dt_max_us': float(np.max(np.diff(t))) if len(t) > 1 else None,
                'threshold_dbm': threshold, 'peaks_us': peaks.tolist(),
                'train_us': train, 'train_period_us': period,
                'train_max_residual_us': residual,
            }
            rows.append(row)
            offset_rows.append(row)
        quiet_chains = sum(bool(r['train_us']) for r in offset_rows if r['tx'] is None)
        transmitters = {}
        for tx in sorted({r['tx'] for r in offset_rows if r['tx'] is not None}):
            arm = [r for r in offset_rows if r['tx'] == tx]
            passing = sum(bool(r['train_us']) for r in arm)
            transmitters[tx] = {
                'captures': len(arm), 'chains': passing,
                'received': sum(r['received'] is True for r in arm),
                'qualified': (len(arm) >= 8 and passing / len(arm) >= 0.75
                              and len(quiet) >= 2 and quiet_chains == 0),
            }
        offsets.append({
            'offset_khz': offset, 'threshold_dbm': threshold,
            'quiet_percentiles_dbm': dict(zip(('p0', 'p50', 'p99', 'p100'), percentiles.tolist())),
            'quiet_captures': len(quiet), 'quiet_chains': quiet_chains,
            'transmitters': transmitters,
            'qualified': bool(transmitters) and all(x['qualified'] for x in transmitters.values()),
        })
    return {
        'raw_validation': 'passed: status, sequence, offset, raw lengths, increasing timestamps',
        'criteria': {
            'threshold': 'quiet p99 + 10 dB per offset; samples strictly above threshold',
            'pulse': 'at least 2 samples, merge gaps <=250 us, linear-power weighted centroid',
            'chain': 'first qualifying 6 consecutive peaks; fitted period 4096 +/-100 us; '
                     'max residual <=150 us; adjacent intervals 3800..4400 us; '
                     'sample gaps within chain <=250 us; start <=20 ms after first pulse',
            'qualification': 'at least 8 captures per TX/offset, >=75% chains, '
                             'at least 2 quiet captures with zero chains',
        },
        'qualified': all(offset['qualified'] for offset in offsets),
        'offsets': offsets, 'rows': rows,
    }


def plot(captures, result, path):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    thresholds = {o['offset_khz']: o['threshold_dbm'] for o in result['offsets']}
    fig, axes = plt.subplots(len(captures), 1, figsize=(16, len(captures) * 1.8),
                             sharex=True, squeeze=False)
    for (_, attempt, t, y), ax in zip(captures, axes[:, 0]):
        offset = attempt['capture']['offset_khz']
        ax.plot(t / 1000, y, lw=0.6)
        ax.axhline(thresholds[offset], c='gray', ls=':', lw=0.5)
        ax.set_ylim(-125, -25)
        ax.set_ylabel(f'{attempt["seq"]} {attempt["tx"]}\n{offset:+} kHz')
    axes[-1, 0].set_xlabel('ms from capture start (device timestamps)')
    fig.tight_layout()
    fig.savefig(path, dpi=100)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('captures', nargs='+', type=Path)
    parser.add_argument('--out', required=True, type=Path, help='output prefix')
    parser.add_argument('--plot', action='store_true', help='also render PNG; needs matplotlib')
    args = parser.parse_args()
    captures = load_captures(args.captures)
    result = analyze(captures)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    output = Path(str(args.out) + '.analysis.json')
    output.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    if args.plot:
        plot(captures, result, Path(str(args.out) + '.png'))
    print(json.dumps({key: value for key, value in result.items() if key != 'rows'}, indent=2))
    print(f'Analysis: {output}')


if __name__ == '__main__':
    main()
