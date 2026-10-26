#!/usr/bin/env python3
"""Validate recording management and on-screen GPU thumbnails in an isolated recording root."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=Path)
    parser.add_argument('h264', type=Path)
    parser.add_argument('h265', type=Path)
    parser.add_argument('thumbnail_fixture', type=Path)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--port', type=int, default=8599)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    config = json.loads(args.config.read_text())
    for key in ('model', 'single_model', 'model_info'):
        config[key] = str((args.config.parent / config[key]).resolve())
    config['retention_minutes'] = 0  # Validator applies/tests 1 minute after pinning an expired file.
    config['recording_root'] = str((args.output / 'recordings').resolve())
    config['segment_seconds'] = 10
    config['sources'] = config['sources'][:2]
    if not config['sources']:
        parser.error('at least one source is required')
    for i, source in enumerate(config['sources']):
        source['uri'] = f'rtsp://127.0.0.1:{args.port}/cam{i}'
        source['selected'] = True
    path = args.output / 'config.json'
    path.write_text(json.dumps(config, indent=2) + '\n')
    path.chmod(0o600)
    build = args.build_dir.resolve()
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = ':'.join(str(build / p) for p in ('', 'src/gst', 'src/media', 'src/streaming', 'examples/nvr')) + ':' + env.get('LD_LIBRARY_PATH', '')
    env['GST_PLUGIN_PATH'] = str(build / 'plugins') + ':' + env.get('GST_PLUGIN_PATH', '')
    env.setdefault('QT_QPA_PLATFORM', 'wayland-egl')
    server_env = env.copy()
    server_env.pop('LD_PRELOAD', None)
    env['LD_PRELOAD'] = str(build / 'src/gst/libamdva_copy_audit.so')
    server = None
    with (args.output / 'server.log').open('w') as server_log, (args.output / 'nvr.log').open('w') as log, (args.output / 'observations.jsonl').open('w') as observations:
        try:
            server = subprocess.Popen([str(build / 'examples/nvr/amdva-nvr-rtsp-server'), str(args.h264.resolve()), str(args.h265.resolve()), str(args.port)], env=server_env, stdout=server_log, stderr=server_log)
            time.sleep(2)
            result = subprocess.run([str(build / 'examples/nvr/amdva-vms-management-validate'), str(path), str(args.thumbnail_fixture.resolve())], env=env, stdout=observations, stderr=log, timeout=110)
            if result.returncode:
                raise RuntimeError(f'GUI validator failed: {result.returncode}; see {args.output / "nvr.log"}')
            summary = json.loads(Path(str(path) + '-management-results.json').read_text())
            summary['passed'] = summary['audit_errors'] == 0
            (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
            print(json.dumps(summary))
            return 0 if summary['passed'] else 1
        finally:
            if server is not None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()


if __name__ == '__main__':
    raise SystemExit(main())
