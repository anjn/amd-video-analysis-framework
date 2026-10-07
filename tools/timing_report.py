#!/usr/bin/env python3
"""Summarize rolling telemetry windows; no operational pass/fail thresholds.
P99 summaries are statistics of window P99 values, not a whole-run percentile.
"""
import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


def summary(values):
    return {'samples': len(values), 'minimum': min(values),
            'median': statistics.median(values), 'maximum': max(values)} if values else None


def report(directory, start):
    last = {}
    generations = set()
    latency = defaultdict(list)
    fps = defaultdict(list)
    intervals = defaultdict(list)
    rows = 0
    legacy_display, legacy_ai = [], []
    with (directory / 'telemetry.jsonl').open() as stream:
        for line in stream:
            row = json.loads(line)
            for item in row.get('telemetry', []):
                last[item['source']] = item
                generations.add((item['source'], item['generation']))
            if row['elapsed_s'] < start:
                continue
            rows += 1
            for channel in row.get("channels", []):
                if channel.get("slot", -1) >= 0 and channel.get("live"):
                    legacy_display.append(channel["display_fps"])
                    legacy_ai.append(channel["ai_fps"])
            for item in row.get('telemetry', []):
                if not item['enabled']:
                    continue
                for group in ('continuous', 'background'):
                    for name, value in item[group]['latency'].items():
                        if value['samples']:
                            latency[f'{group}.{name}'].append(value['p99_ms'])
                    for name, value in item[group]['stages'].items():
                        if value['supported'] and value['active'] and value['age_ms'] is not None:
                            fps[f'{group}.{name}'].append(value['fps'])
                            if value['interval']['samples']:
                                intervals[f'{group}.{name}'].append(value['interval']['p99_ms'])
    resources = []
    with (directory / 'resources.csv').open() as stream:
        for row in csv.DictReader(stream):
            if float(row['elapsed_s']) >= start:
                resources.append({k: float(v) for k, v in row.items() if v and v != 'None'})
    cpu = None
    if len(resources) >= 2:
        first, final = resources[0], resources[-1]
        cpu = 100 * (final['cpu_seconds'] - first['cpu_seconds']) / (final['elapsed_s'] - first['elapsed_s'])
    return {'artifact_directory': str(directory), 'window_start_s': start,
            'observed_snapshots': rows, 'source_generations': len(generations),
            'notes': 'Window P99 summaries are not whole-run percentiles. CPU percent is relative to one logical core. No latency or FPS pass/fail classification.',
            'rolling_window_p99_ms': {k: summary(v) for k, v in latency.items()},
            'rolling_interval_p99_ms': {k: summary(v) for k, v in intervals.items()},
            'observed_fps': {k: summary(v) for k, v in fps.items()},
            'legacy_selected_display_fps': summary(legacy_display),
            'legacy_selected_ai_fps': summary(legacy_ai),
            'process_cpu_percent': cpu,
            'rss_kib': summary([r['rss_kib'] for r in resources]),
            'gpu_alloc_kib': summary([r['gpu_alloc_kib'] for r in resources if 'gpu_alloc_kib' in r]),
            'last_by_source': last}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--start', type=float, default=0)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(report(args.directory, args.start), indent=2) + '\n')


if __name__ == '__main__':
    main()
