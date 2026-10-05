#!/usr/bin/env python3
"""Exercise the NVR against encoded RTSP fixtures with resource guards."""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from soak import gpu_alloc_kib


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('config', type=Path)
    parser.add_argument('h264', type=Path)
    parser.add_argument('h265', type=Path)
    parser.add_argument('--seconds', type=int, default=3600)
    parser.add_argument('--cycles', type=int, default=0)
    parser.add_argument('--port', type=int, default=8555)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--swap-codecs', action='store_true', help='Change codec at the same URL after server restart')
    parser.add_argument('--restart-at', type=int, default=0)
    args = parser.parse_args()
    if args.seconds < 40 or args.seconds < 30 + args.cycles * 2:
        parser.error('allow startup, 2 seconds per cycle and recovery')
    root = Path(__file__).resolve().parents[1]
    if args.output.exists():
        parser.error('select a new output directory')
    args.output.mkdir(parents=True)
    config = json.loads(args.config.read_text())
    for field in ('model', 'single_model', 'model_info'):
        config[field] = str((args.config.parent / config[field]).resolve())
    config['recording_root'] = str((args.output / 'recordings').resolve())
    for i, source in enumerate(config['sources']):
        source['uri'] = f'rtsp://127.0.0.1:{args.port}/cam{i}'
    local_config = args.output / 'config.json'
    local_config.write_text(json.dumps(config, indent=2))
    local_config.chmod(0o600)
    env = os.environ.copy()
    env.setdefault('WAYLAND_DISPLAY', 'wayland-0')
    env.setdefault('QT_QPA_PLATFORM', 'wayland-egl')
    server_command = [str(root / 'build/src/nvr/amdva-nvr-rtsp-server'),
                      str(args.h264.resolve()), str(args.h265.resolve()), str(args.port)]
    env['LD_PRELOAD'] = str(root / 'build/src/gst/libamdva_copy_audit.so')
    server_env = env.copy()
    server_env.pop('LD_PRELOAD', None)
    warmup = max(60, 30 + args.cycles * 2)
    baseline = None
    growth = {'rss_kib': 0, 'gpu_alloc_kib': 0, 'fds': 0}
    failure = None
    restarted = False
    server = process = None
    with (args.output / 'server.log').open('w') as server_log, \
            (args.output / 'nvr.log').open('w') as log, \
            (args.output / 'telemetry.jsonl').open('w') as telemetry, \
            (args.output / 'resources.csv').open('w') as resource:
        writer = csv.DictWriter(resource, fieldnames=['elapsed_s', 'rss_kib', 'gpu_alloc_kib', 'fds'])
        writer.writeheader()
        try:
            server = subprocess.Popen(server_command, env=server_env, stdout=server_log, stderr=server_log)
            time.sleep(1)
            if server.poll() is not None:
                raise RuntimeError('RTSP fixture server exited')
            process = subprocess.Popen([str(root / 'build/src/nvr/amdva-nvr-validate'),
                                        str(local_config), str(args.seconds), str(args.cycles)],
                                       env=env, stdout=telemetry, stderr=log)
            started = time.monotonic()
            while process.poll() is None:
                elapsed = time.monotonic() - started
                if elapsed > args.seconds + 30:
                    raise RuntimeError('NVR exceeded the shutdown deadline')
                if args.restart_at and elapsed >= args.restart_at and not restarted:
                    stop(server)
                    time.sleep(3)
                    server = subprocess.Popen(server_command + (['--swap-codecs'] if args.swap_codecs else []), env=server_env, stdout=server_log, stderr=server_log)
                    restarted = True
                try:
                    status = Path(f'/proc/{process.pid}/status').read_text()
                    rss = int(next(line.split()[1] for line in status.splitlines() if line.startswith('VmRSS:')))
                    row = {'elapsed_s': elapsed, 'rss_kib': rss,
                           'gpu_alloc_kib': gpu_alloc_kib(process.pid),
                           'fds': len(list(Path(f'/proc/{process.pid}/fd').iterdir()))}
                    writer.writerow(row)
                    resource.flush()
                    if elapsed >= warmup:
                        if baseline is None:
                            baseline = row
                        for key in growth:
                            if row[key] is None:
                                raise RuntimeError('GPU allocation counter missing')
                            growth[key] = max(growth[key], row[key] - baseline[key])
                        # Finalizers may briefly hold one extra FD per channel.
                        if growth['rss_kib'] > 65536 or growth['gpu_alloc_kib'] > 65536 or growth['fds'] > 2 * len(config['sources']) + 4:
                            raise RuntimeError('Resource growth exceeded the configured guard')
                except (FileNotFoundError, ProcessLookupError, StopIteration):
                    pass
                time.sleep(5)
            if process.returncode:
                failure = f'NVR validator exit {process.returncode}'
        except Exception as error:
            failure = str(error)
        finally:
            if process is not None:
                stop(process)
            if server is not None:
                stop(server)
    lines = (args.output / 'telemetry.jsonl').read_text().splitlines()
    samples = [json.loads(line) for line in lines]
    last = samples[-1] if samples else None
    recovery_s = None
    if restarted:
        lost = next((row for row in samples if row['elapsed_s'] >= args.restart_at - 5
                     and row['live'] < len(config['sources'])), None)
        recovered = next((row for row in samples if lost and row['elapsed_s'] > lost['elapsed_s']
                          and row['live'] == len(config['sources'])
                          and row['recording'] == len(config['sources'])
                          and row['selected'] == 16), None)
        if lost and recovered:
            recovery_s = recovered['elapsed_s'] - lost['elapsed_s']
        if recovery_s is None or recovery_s > 60:
            failure = failure or 'RTSP receive/record recovery was not observed within 60 seconds'
    if last and last['copy_errors']:
        failure = 'Copy audit failed'
    recordings = list((args.output / 'recordings').glob('*.mkv'))
    invalid = []
    for path in recordings:
        result = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'stream=codec_name,width,height',
                                 '-of', 'json', str(path)], capture_output=True, text=True)
        if result.returncode or not json.loads(result.stdout or '{}').get('streams'):
            invalid.append(path.name)
    if invalid:
        detail = f'{len(invalid)} invalid MKV files'
        failure = f'{failure}; {detail}' if failure else detail
    summary = {'passed': failure is None, 'failure': failure, 'duration_s': args.seconds,
               'cycles': args.cycles, 'server_restarted': restarted, 'recovery_s': recovery_s,
               'warmup_s': warmup,
               'maximum_growth': growth, 'recordings_checked': len(recordings),
               'invalid_recordings': invalid, 'last': last}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({k: v for k, v in summary.items() if k != 'last'}))
    return 0 if failure is None else 2


if __name__ == '__main__':
    raise SystemExit(main())
