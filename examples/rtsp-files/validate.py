#!/usr/bin/python3
"""Exercise file-list RTSP routes, looping, original FPS, B frames and clients."""
import argparse
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import time
from fractions import Fraction
import gi

gi.require_version('Gst', '1.0')
from gi.repository import Gst


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, default=Path('build/examples/rtsp-files/amdva-rtsp-files'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--hardware-decode', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    server_path = str(args.server.resolve())
    root = args.output.resolve()
    avc, hevc, raw = root / '映像 avc.mp4', root / 'clip hevc.mkv', root / 'raw.h264'
    common = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i']
    color = ['-pix_fmt', 'yuv420p', '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709', '-color_range', 'tv']
    subprocess.run(common + ['testsrc2=size=640x360:rate=25', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '2', '-c:v', 'libx264', '-g', '25', '-bf', '2', '-c:a', 'aac'] + color + [str(avc)], check=True)
    subprocess.run(common + ['testsrc2=size=640x360:rate=30000/1001', '-t', '2', '-c:v', 'libx265', '-x265-params', 'keyint=30:min-keyint=30:scenecut=0:bframes=3:pools=2:log-level=error'] + color + [str(hevc)], check=True)
    subprocess.run(common + ['testsrc2=size=640x360:rate=24', '-t', '2', '-c:v', 'libx264', '-g', '24', '-bf', '0'] + color + [str(raw)], check=True)
    vfr = root / 'variable fps.mp4'
    subprocess.run(common + ['testsrc2=size=640x360:rate=30', '-t', '2', '-vf', "select='if(lt(t,1),not(mod(n,2)),1)'", '-fps_mode', 'vfr', '-c:v', 'libx264', '-g', '30', '-bf', '2'] + color + [str(vfr)], check=True)
    probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=avg_frame_rate', '-of', 'json', str(vfr)], text=True))
    vfr_fps = float(Fraction(probe['streams'][0]['avg_frame_rate']))
    listing = root / 'videos.txt' 
    listing.write_text('\ufeff# input paths relative to this list\r\n\r\n' + avc.name + '\r\n' + hevc.name + '\r\n' + raw.name + '\r\n' + avc.name + '\r\n' + vfr.name + '\r\n', encoding='utf-8')
    empty = root / 'empty.txt'
    empty.write_text('# no videos\n\n')
    broken = root / 'broken.bin'
    broken.write_bytes(b'not a movie')
    for command in ([server_path], [server_path, '--port', '0'], [server_path, '--list', str(empty)], [server_path, str(broken)], [server_path, str(root / 'missing.mp4')]):
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        require(result.returncode != 0, f'Invalid command succeeded: {command}')
    Gst.init(None)
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1', 0))
        port = reserved.getsockname()[1]
    clients = []
    specs = [(0, 'h264', 25, 'tcp'), (1, 'h265', 30000 / 1001, 'tcp'), (2, 'h264', 24, 'udp'), (3, 'h264', 25, 'tcp'), (0, 'h264', 25, 'tcp'), (4, 'h264', vfr_fps, 'tcp')]
    with (root / 'server.log').open('w') as log:
        server = subprocess.Popen([server_path, '--list', str(listing), '--port', str(port)], stdout=subprocess.PIPE, stderr=log, text=True)
        try:
            require(select.select([server.stdout], [], [], 10)[0], 'Server startup timed out')
            urls = [server.stdout.readline().strip() for _ in range(5)]
            require(urls == [f'rtsp://127.0.0.1:{port}/cam{i}' for i in range(5)], 'Incorrect file-list route mapping; see server.log')
            conflict = subprocess.run([server_path, '--port', str(port), str(avc)], capture_output=True, text=True, timeout=10)
            require(conflict.returncode != 0, 'Port conflict was ignored')

            def connect(spec):
                route, codec, fps, transport = spec
                pipeline = Gst.parse_launch(f'rtspsrc location={urls[route]} protocols={transport} latency=150 ! rtp{codec}depay ! {codec}parse ! ' + (f'va{codec}dec ! video/x-raw(memory:VAMemory),format=NV12 ! ' if args.hardware_decode else '') + 'appsink name=result sync=false max-buffers=8 drop=false')
                pipeline.set_state(Gst.State.PLAYING)
                return {'pipeline': pipeline, 'sink': pipeline.get_by_name('result'), 'route': route, 'fps_expected': fps, 'transport': transport, 'frames': 0, 'min_pts': None, 'max_pts': 0, 'last_pts': None, 'presentation_pts': []}

            clients = [connect(spec) for spec in specs]
            started = time.monotonic()
            restarted = False
            peak_rss_kib = peak_fds = 0
            while time.monotonic() - started < 12:
                require(server.poll() is None, 'Server exited while streaming')
                for client in clients:
                    sample = client['sink'].emit('try-pull-sample', 2 * Gst.MSECOND)
                    if sample:
                        buffer = sample.get_buffer()
                        require(buffer.pts != Gst.CLOCK_TIME_NONE, 'Missing output PTS')
                        if args.hardware_decode and client['last_pts'] is not None:
                            require(buffer.pts >= client['last_pts'], 'Decoded PTS went backwards across a loop')
                        caps = sample.get_caps().get_structure(0)
                        require(caps.get_value('width') == 640 and caps.get_value('height') == 360, 'Resolution changed')
                        client['frames'] += 1
                        client['min_pts'] = buffer.pts if client['min_pts'] is None else min(client['min_pts'], buffer.pts)
                        client['max_pts'] = max(client['max_pts'], buffer.pts)
                        client['last_pts'] = buffer.pts
                        if len(client['presentation_pts']) < 45:
                            client['presentation_pts'].append(buffer.pts)
                    message = client['pipeline'].get_bus().pop_filtered(Gst.MessageType.ERROR | Gst.MessageType.EOS)
                    if message:
                        raise RuntimeError(str(message.parse_error()) if message.type == Gst.MessageType.ERROR else 'Unexpected EOS at file boundary')
                if time.monotonic() - started > 6 and not restarted:
                    clients[3]['pipeline'].set_state(Gst.State.NULL)
                    clients[3] = connect(specs[3])
                    restarted = True
                statm = Path(f'/proc/{server.pid}/statm').read_text().split()
                peak_rss_kib = max(peak_rss_kib, int(statm[1]) * os.sysconf('SC_PAGE_SIZE') // 1024)
                peak_fds = max(peak_fds, len(list(Path(f'/proc/{server.pid}/fd').iterdir())))
            summary = []
            for client in clients:
                require(client['frames'] > client['fps_expected'] * 3, 'Too few frames; repeated playback failed')
                span = (client['max_pts'] - client['min_pts']) / Gst.SECOND
                require(span > 4, 'Did not cross enough two-second loop boundaries')
                fps = (client['frames'] - 1) / span
                require(abs(fps / client['fps_expected'] - 1) < 0.04, f'Original FPS not preserved: {fps}')
                summary.append({k: client[k] for k in ('route', 'transport', 'frames', 'fps_expected')} | {'fps_measured': fps, 'media_span_s': span})
            vfr_pts = sorted(clients[-1]['presentation_pts'])
            vfr_intervals = {round((b-a)/(Gst.SECOND/30)) for a,b in zip(vfr_pts,vfr_pts[1:])}
            require({1, 2} <= vfr_intervals, 'Variable frame intervals were flattened')
            result = {'vfr_intervals_30fps_ticks': sorted(vfr_intervals), 'passed': True, 'hardware_decode': args.hardware_decode, 'clients': summary, 'reconnected': restarted, 'port_conflict_rejected': True, 'invalid_inputs_rejected': True, 'peak_server_rss_kib': peak_rss_kib, 'peak_server_fds': peak_fds}
            (root / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(result))
        finally:
            for client in clients:
                client['pipeline'].set_state(Gst.State.NULL)
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
                raise RuntimeError('Server failed to stop after SIGTERM')
            require(server.returncode == 0, f'Server shutdown failed: {server.returncode}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
