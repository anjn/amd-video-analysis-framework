#!/usr/bin/env python3
"""Exercise real headless dashboard RTSP delivery, reconnects and copy/resource guards."""
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from soak import gpu_alloc_kib
from nvr_soak import process_work, stop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=Path('build'))
    parser.add_argument('--port', type=int, default=8586)
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.seconds < 100:
        parser.error('allow at least 100 seconds for reconnect scenarios')
    args.output.mkdir(parents=True, exist_ok=False)
    runtime = args.runtime.resolve()
    binary = runtime / 'amdva-dashboard'
    audit = runtime / 'libamdva_copy_audit.so'
    if not binary.exists():
        binary = runtime / 'examples/resource-dashboard/amdva-dashboard'
        audit = runtime / 'src/gst/libamdva_copy_audit.so'
    env = os.environ.copy()
    env.pop('DISPLAY', None)
    env.pop('WAYLAND_DISPLAY', None)
    env['LD_LIBRARY_PATH'] = str(runtime) + ':' + env.get('LD_LIBRARY_PATH', '')
    env['LD_PRELOAD'] = str(audit)
    child_env = os.environ.copy()
    child_env.pop('LD_PRELOAD', None)
    url = f'rtsp://127.0.0.1:{args.port}/dashboard'
    clients, logs = [], []
    server = None
    failure = None
    baseline = None
    growth = {'rss_kib': 0, 'gpu_alloc_kib': 0, 'fds': 0}
    samples = []
    scenarios = []

    def client(name, seconds, record=True):
        log = (args.output / f'{name}.log').open('w')
        logs.append(log)
        target = str(args.output / f'{name}.mkv') if record else '-'
        command = ['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'warning', '-rtsp_transport', 'tcp',
                   '-i', url, '-t', str(seconds), '-map', '0:v:0', '-c', 'copy']
        if not record:
            command += ['-f', 'null']
        process = subprocess.Popen(command + ['-y', target], env=child_env, stdout=log, stderr=log)
        clients.append(process)
        return process

    with (args.output / 'server.log').open('w') as log, (args.output / 'resources.csv').open('w') as resource:
        writer = csv.DictWriter(resource, fieldnames=['elapsed_s', 'rss_kib', 'gpu_alloc_kib', 'fds', 'cpu_seconds', 'drm_engine_enc_ns', 'drm_engine_vpe_ns'])
        writer.writeheader()
        try:
            server = subprocess.Popen([str(binary), '--bind', '127.0.0.1', '--port', str(args.port), '--duration-seconds', str(args.seconds + 20)], env=env, stdout=log, stderr=log)
            ready = time.monotonic()
            while 'Dashboard ready:' not in (args.output / 'server.log').read_text():
                if server.poll() is not None or time.monotonic() - ready > 30:
                    raise RuntimeError('Dashboard did not start')
                time.sleep(.2)
            started = time.monotonic()
            primary = client('initial', 50)
            secondary = abrupt = resumed = None
            idle_start = idle_frames = None
            captured = False
            while time.monotonic() - started < args.seconds:
                elapsed = time.monotonic() - started
                if server.poll() is not None:
                    raise RuntimeError(f'Server stopped early: {server.returncode}')
                if elapsed >= 10 and secondary is None:
                    secondary = client('shared-client', 15)
                    scenarios.append('second simultaneous RTSP client')
                if primary.poll() is not None and idle_start is None:
                    if primary.returncode:
                        raise RuntimeError('Initial RTSP client failed')
                    if secondary and secondary.poll() is None:
                        raise RuntimeError('Secondary client did not finish')
                    idle_start = elapsed
                    time.sleep(1)
                    values = [json.loads(line) for line in (args.output / 'server.log').read_text().splitlines() if line.startswith('{')]
                    idle_frames = values[-1]['frames'] if values else None
                if idle_start is not None and elapsed - idle_start >= 10 and resumed is None:
                    values = [json.loads(line) for line in (args.output / 'server.log').read_text().splitlines() if line.startswith('{')]
                    # Allow in-flight frames and the first status snapshot, but no continued idle encoding.
                    if idle_frames is not None and values[-1]['frames'] - idle_frames > 15:
                        raise RuntimeError('Encoder kept producing after all clients disconnected')
                    resumed = client('reconnected', args.seconds - elapsed + 10)
                    scenarios.append('all clients disconnected, idle encoding stopped, new connection resumed')
                if elapsed >= 75 and abrupt is None:
                    abrupt = client('abrupt-client', 20, record=False)
                if elapsed >= 83 and abrupt and abrupt.poll() is None:
                    abrupt.kill()
                    abrupt.wait()
                    scenarios.append('abrupt client disconnect with another client active')
                if elapsed >= 90 and not captured:
                    capture = subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-rtsp_transport', 'tcp', '-i', url, '-frames:v', '1', '-update', '1', '-y', str(args.output / 'frame.png')], env=child_env, capture_output=True, timeout=20)
                    if capture.returncode:
                        raise RuntimeError('Diagnostic decode/capture failed')
                    captured = True
                status = Path(f'/proc/{server.pid}/status').read_text()
                row = {'elapsed_s': elapsed, 'rss_kib': int(next(s.split()[1] for s in status.splitlines() if s.startswith('VmRSS:'))),
                       'gpu_alloc_kib': gpu_alloc_kib(server.pid), 'fds': len(list(Path(f'/proc/{server.pid}/fd').iterdir()))}
                row.update(process_work(server.pid))
                writer.writerow(row)
                resource.flush()
                samples.append(row)
                if elapsed >= 240:
                    if baseline is None:
                        baseline = row
                    for key in growth:
                        growth[key] = max(growth[key], row[key] - baseline[key])
                    if growth['rss_kib'] > 65536 or growth['gpu_alloc_kib'] > 65536 or growth['fds'] > 44:
                        raise RuntimeError('Existing memory/FD growth guards exceeded')
                if resumed and resumed.poll() is not None:
                    raise RuntimeError('Reconnected RTSP client ended early')
                time.sleep(5)
            if not resumed or not captured or len(scenarios) != 3:
                raise RuntimeError('Scenarios incomplete')
        except Exception as e:
            failure = str(e)
        finally:
            for process in clients:
                if process.poll() is None:
                    process.send_signal(signal.SIGINT)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        stop(process)
            if server:
                stop(server)
            for handle in logs:
                handle.close()
    text = (args.output / 'server.log').read_text()
    if server and server.returncode:
        failure = failure or f'Server exit {server.returncode}'
    if 'Dashboard error:' in text or 'audit-errors=0' not in text or 'prohibited' in text or 'CRITICAL' in text:
        failure = failure or 'Server/copy audit failure'
    videos = []
    for path in args.output.glob('*.mkv'):
        probe = subprocess.run(['ffprobe', '-v', 'error', '-count_frames', '-show_entries', 'stream=codec_name,width,height,avg_frame_rate,nb_read_frames:format=duration', '-of', 'json', str(path)], capture_output=True, text=True)
        data = json.loads(probe.stdout or '{}')
        streams = data.get('streams', [])
        if probe.returncode or not streams or streams[0].get('codec_name') != 'h264' or streams[0].get('width') != 1920 or int(streams[0].get('nb_read_frames', '0')) < 10:
            failure = failure or f'Invalid recording: {path.name}'
        videos.append({'file': path.name, **data})
    summary = {'passed': failure is None, 'failure': failure, 'seconds': args.seconds, 'scenarios': scenarios,
               'maximum_growth': growth, 'warmup_seconds': 240, 'resource_samples': len(samples),
               'videos': videos, 'copy_audit_violations': 0 if 'audit-errors=0' in text and 'prohibited' not in text else None}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if failure is None else 2


if __name__ == '__main__':
    raise SystemExit(main())
