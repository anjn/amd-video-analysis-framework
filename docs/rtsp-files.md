# Video files as RTSP cameras

`amdva-rtsp-files` publishes each input file as a separate camera URL, looping
that file indefinitely. It reads encoded video incrementally and performs no
video decoding or encoding. The executable has no Qt or AMD GPU dependency.
MP4/MOV and Matroska H.264/H.265 video are supported by the supplied runtime.
Audio, subtitles and additional video tracks are discarded; the first supported
video track is selected. The list is fixed at startup.

## Run

```bash
source scripts/env.sh
build/src/rtsp_files/amdva-rtsp-files --list examples/rtsp-files.txt --port 8558
```

The supplied list refers to the generated NVR fixtures. For your own files,
create a UTF-8 text file, one path per line:

```text
# Paths relative to this list file, or absolute paths
/home/amd/Videos/entrance.mp4
/home/amd/Videos/parking.mkv
```

```bash
build/src/rtsp_files/amdva-rtsp-files --list /path/to/videos.txt --port 8558
# Equivalent positional input:
build/src/rtsp_files/amdva-rtsp-files --port 8558 "/path/to/video one.mp4" /path/to/video2.mkv
```

The program prints these URLs on stdout, in input order:

```text
rtsp://127.0.0.1:8558/cam0
rtsp://127.0.0.1:8558/cam1
```

Add those URLs to the VMS Cameras page, or use them as source URIs in an NVR
configuration. The port must be free. `--port` defaults to 8554. Source filenames,
codecs, resolutions and detected FPS are logged on stderr. Missing files, empty
lists, unsupported video and an occupied port cause a nonzero exit status.

A list can contain blank lines, full-line `#` comments, spaces in filenames,
a UTF-8 BOM and CRLF line endings. Do not quote or escape list entries; shell
quoting applies only to command-line arguments. Leading/trailing whitespace on
each list entry is trimmed. Paths are relative to the list's directory, rather
than the working directory. Repeat a file on multiple lines to give it multiple
independent camera URLs. Multiple `--list` arguments and positional paths can
be combined in argument order. `--` ends option parsing.

To change the mount prefix or allow connections from another machine:

```bash
build/src/rtsp_files/amdva-rtsp-files --list videos.txt \
  --address 0.0.0.0 --port 8558 --prefix /video
```

This creates `/video0`, `/video1`, etc. Use the server's LAN address in remote
clients. The default bind address is `127.0.0.1`. RTSP-over-TCP and UDP are both
available. There is no authentication or TLS; this is a local/LAN demo input
tool. Ctrl+C or SIGTERM stops the server and closes its clients.

## Timing and resource use

Container PTS/DTS and frame durations are retained relative to the stream's
origin. Each loop advances the media timeline rather than resetting RTP time.
The loop span covers both presentation and decode timestamp ranges; edit-list
or reorder padding can cause a brief boundary hold. B-frame presentation
ordering is preserved. When a container omits DTS (common
in Matroska), a separate decode clock is synthesized from AU durations.
Variable frame timing is preserved when present; the tool does not force all
inputs to 30 fps.

Raw Annex-B `.h264`/`.h265` streams can also be used. Prefer MP4/MKV for B-frame
video, since raw elementary files do not carry container presentation timing.
For raw files without timestamps, the parser-reported FPS is used, or
`--fallback-fps N` (default 30) when FPS is unknown. A raw stream without
presentation timing must have no B frames. Files should begin at a decodable
keyframe. The tool does not resize or transcode content to match NVR input
limits, and input files must not be modified while serving them.

Every connected URL has one producer shared by its clients. New clients of an
active URL join its current playback position and may wait for a keyframe.
Different URLs have independent producers, even when their filenames match.
An unused URL does not continuously read the video. Preflight and replay use
bounded appsink/queue buffering instead of retaining the entire file; container
metadata and a few compressed access units still consume memory. Files are
reopened at loop boundaries, including raw inputs that cannot seek by time.

## Build and validation

The normal project enables `AMDVA_BUILD_RTSP_FILES=ON`. The target is independent
of `AMDVA_BUILD_NVR`, `AMDVA_BUILD_STREAMING` and the dashboard. Bootstrap enables
the `isomp4` plugin for MP4/MOV, alongside the existing Matroska support. It is
also possible to build only this tool with a C++17 compiler and GStreamer
1.28.2 development files (core, app and RTSP server):

```bash
source scripts/env.sh
cmake -S src/rtsp_files -B artifacts/rtsp-files-only
cmake --build artifacts/rtsp-files-only -j4
ctest --test-dir artifacts/rtsp-files-only --output-on-failure
```

The runtime needs `parsebin`, `filesrc`, `appsink`, `appsrc`, `identity`,
`fakesink`, H.264/H.265 parsers and RTP payloaders, plus the relevant demuxer
(`qtdemux` or `matroskademux`). Unsupported codecs are rejected rather than
silently transcoded.

For a functional test using generated two-second fixtures:

```bash
source scripts/env.sh
/usr/bin/python3 tools/rtsp_files_validate.py \
  --server build/src/rtsp_files/amdva-rtsp-files \
  --output artifacts/rtsp-files-check --hardware-decode
```

This requires FFmpeg/FFprobe with libx264/libx265/AAC encoding, Python GI/GStreamer, and
VA decoders for `--hardware-decode`. Omit that flag for encoded-only clients on
a machine without a GPU. It tests MP4 with audio, Matroska, raw video, B frames,
25/29.97/24 fps and variable frame intervals, repeated paths, UTF-8/list-relative
paths, looping, shared
clients, TCP/UDP, reconnect, invalid inputs, port conflicts and shutdown.
The test records decoded/received frame counts and timing, plus peak server
RSS and FD count; a short run does not establish long-term resource stability.
See [the validation record](rtsp-files-validation-results.json).
