#!/usr/bin/env python3
"""Run the VMS GUI scenarios with local RTSP fixtures and recording/resource guards."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'tools'))
from soak import gpu_alloc_kib, process_work, stop  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=Path)
    parser.add_argument('h264', type=Path)
    parser.add_argument('h265', type=Path)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--restart-at', type=int, default=180)
    parser.add_argument('--port', type=int, default=8576)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.seconds < 130:
        parser.error('allow at least 130 seconds for the GUI scenarios')
    args.output.mkdir(parents=True, exist_ok=False)
    config = json.loads(args.config.read_text())
    for key in ('model', 'single_model', 'model_info'):
        config[key] = str((args.config.parent / config[key]).resolve())
    config['recording_root'] = str((args.output / 'recordings').resolve())
    config['segment_seconds'] = 10
    for i, source in enumerate(config['sources']):
        source['uri'] = f'rtsp://127.0.0.1:{args.port}/cam{i}'
        source['selected'] = i < 16
    path = args.output / 'config.json'
    path.write_text(json.dumps(config, indent=2) + '\n')
    path.chmod(0o600)
    build = args.build_dir.resolve()
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = ':'.join(str(build / p) for p in ('', 'src/gst', 'src/media', 'src/streaming', 'examples/nvr')) + ':' + env.get('LD_LIBRARY_PATH', '')
    env['GST_PLUGIN_PATH'] = str(build / 'plugins') + ':' + env.get('GST_PLUGIN_PATH', '')
    env.setdefault('WAYLAND_DISPLAY', 'wayland-0')
    env.setdefault('QT_QPA_PLATFORM', 'wayland-egl')
    server_env = env.copy()
    server_env.pop('LD_PRELOAD', None)
    env['LD_PRELOAD'] = str(build / 'src/gst/libamdva_copy_audit.so')
    server_command = [str(build / 'examples/nvr/amdva-nvr-rtsp-server'), str(args.h264.resolve()), str(args.h265.resolve()), str(args.port)]
    server = process = None
    failure = None
    baseline = None
    growth = {'rss_kib': 0, 'gpu_alloc_kib': 0, 'fds': 0}
    restarted = False
    with (args.output / 'server.log').open('w') as server_log, (args.output / 'nvr.log').open('w') as log, (args.output / 'observations.jsonl').open('w') as observations, (args.output / 'resources.csv').open('w') as resource:
        writer = csv.DictWriter(resource, fieldnames=['elapsed_s', 'rss_kib', 'gpu_alloc_kib', 'fds', 'cpu_seconds', 'drm_engine_enc_ns', 'drm_engine_vpe_ns'])
        writer.writeheader()
        try:
            server = subprocess.Popen(server_command, env=server_env, stdout=server_log, stderr=server_log)
            time.sleep(2)
            process = subprocess.Popen([str(build / 'examples/nvr/amdva-vms-validate'), str(path), str(args.seconds)], env=env, stdout=observations, stderr=log)
            started = time.monotonic()
            while process.poll() is None:
                elapsed = time.monotonic() - started
                if args.restart_at and not restarted and elapsed >= args.restart_at:
                    stop(server)
                    time.sleep(3)
                    server = subprocess.Popen(server_command + ['--swap-codecs'], env=server_env, stdout=server_log, stderr=server_log)
                    restarted = True
                try:
                    status = Path(f'/proc/{process.pid}/status').read_text()
                    row = {'elapsed_s': elapsed, 'rss_kib': int(next(line.split()[1] for line in status.splitlines() if line.startswith('VmRSS:'))), 'gpu_alloc_kib': gpu_alloc_kib(process.pid), 'fds': len(list(Path(f'/proc/{process.pid}/fd').iterdir()))}
                    row.update(process_work(process.pid))
                    writer.writerow(row)
                    resource.flush()
                    if elapsed >= 240:
                        if baseline is None:
                            baseline = row
                        for key in growth:
                            growth[key] = max(growth[key], row[key] - baseline[key])
                        if growth['rss_kib'] > 65536 or growth['gpu_alloc_kib'] > 65536 or growth['fds'] > 44:
                            raise RuntimeError('Resource growth exceeded existing NVR guards')
                except (FileNotFoundError, ProcessLookupError, StopIteration):
                    pass
                time.sleep(5)
            if process.returncode:
                failure = f'VMS validator exit {process.returncode}'
        except Exception as error:
            failure = str(error)
        finally:
            if process:
                stop(process)
            if server:
                stop(server)
    last = None
    for line in (args.output / 'observations.jsonl').read_text().splitlines():
        try:
            last = json.loads(line)
        except ValueError:
            pass
    if not last or last['elapsed_s'] < args.seconds - 2:
        failure = failure or 'Scenarios ended early'
    invalid = []
    recordings = list((args.output / 'recordings').glob('*.mkv'))
    for recording in recordings:
        result = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'stream=codec_name,width,height', '-of', 'json', str(recording)], capture_output=True, text=True)
        if result.returncode or not json.loads(result.stdout or '{}').get('streams'):
            invalid.append(recording.name)
    if invalid:
        failure = failure or 'Invalid recording files'
    summary = {'passed': failure is None, 'failure': failure, 'duration_s': args.seconds, 'server_restarted': restarted, 'warmup_s': 240, 'maximum_growth': growth, 'recordings_checked': len(recordings), 'invalid_recordings': invalid, 'last': last}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({k: v for k, v in summary.items() if k != 'last'}))
    return 0 if failure is None else 2


if __name__ == '__main__':
    raise SystemExit(main())
