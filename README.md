# AMD Video Analysis Framework

GStreamer plugins for AMD APU video analytics, with selected Intel DL Streamer
elements for tracking and JSON output. The initial implementation targets one
H.264 stream and YOLOv8n COCO detection on Linux.

```text
H.264 → vah264dec → amdpreproc → amdmigraphx → gvatrack
      → gvametaconvert → gvametapublish → fakesink
```

`amdpreproc` retains the original NV12 VA video memory and timestamps. It uses
the dedicated VPE to scale and convert an internal surface to linear BGRA, then
HIP generates an RGB float32 NCHW tensor with letterbox padding. `amdmigraphx`
consumes that device allocation directly, runs inference and class-aware NMS on
the GPU, and attaches GstAnalytics detections in original-image coordinates.
The only per-frame CPU readback is a bounded 7,204-byte detection structure.

Strict mode rejects CPU image transfers, GPU memcpy staging, unsupported memory
layouts and scaling without observed VPE engine activity. See
[architecture and interfaces](docs/architecture.md) for the exact constraints
and [validation](docs/validation.md) for evidence and measurement limits.

Local validation passed six tests, the six-detection CPU reference comparison,
and a one-hour 1080p30 run (107970 frames, no observed prohibited copies,
P95 processing latency 10.412 ms). See the
[recorded results](docs/validation-results.json) for resource limits and scope.

## Build on the host

The validated hardware is Ryzen AI 9 HX 370 / Radeon 890M, **gfx1150**, VPE 6.1.0.
The original login environment forced gfx1151; `scripts/env.sh` removes that
override. The software baseline is Ubuntu 24.04, kernel 6.17.0-1032-oem and Mesa
25.2.8. GPU device access through `/dev/dri/renderD128` and `/dev/kfd` is required.

Host prerequisites: Python 3.12+, `uv`, C/C++ compilers, CMake 3.22+, pkg-config,
git, dpkg-deb, apt-get, m4, and GLib development files (`glib-2.0 >= 2.64`).
FFmpeg with libx264 is used for test fixtures. The bootstrap installs to `.deps/`
without sudo or system library replacement. It downloads several GB of ROCm
packages; extracted libraries require substantially more disk space.

```bash
python3 scripts/bootstrap.py --jobs 8
source scripts/env.sh
ctest --test-dir build --output-on-failure
build/amdva-probe
build/src/gst/amdva-validate-preproc
```

Dependencies are pinned with SHA256 hashes in [dependencies.lock.json](dependencies.lock.json):
GStreamer 1.28.2 with DL Streamer's analytics extension patch, DL Streamer
v2026.2.0, ROCm 7.2.0 / MIGraphX 2.15.0, and OpenVINO 2026.2.0. The selected
DL Streamer build retains its OpenVINO dependency; inference uses MIGraphX.
`python3 scripts/bootstrap.py --check` verifies cached dependency downloads.
Do not reinstall or rebuild loaded libraries during a running pipeline.

## Prepare a model and run

Use a static YOLOv8 COCO ONNX with `[1,3,640,640]` RGB input and
`[1,84,8400]` output. Model metadata follows the supplied
[example](examples/yolov8n.json); the 80-line label file sits beside that JSON.
Models and label files from the background project can be used directly.

```bash
source scripts/env.sh
mkdir -p artifacts
build/src/gst/amdva-compile-model /path/yolov8n.onnx artifacts/yolov8n.mxr \
  > artifacts/model-compile.log 2>&1
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/src/gst/amdva-run /path/input.h264 artifacts/yolov8n.mxr \
  /path/yolov8n.json --events=artifacts/events.jsonl
```

Model preparation uses FP16 internal computation and `offload_copy=false`.
It writes a SHA256 and GPU identity manifest beside the MXR; runtime rejects a
missing, mismatched or incompatible manifest. Pre-existing MXR files must be
recompiled. Model compilation is separate from runtime pipeline control.

`amdva-run` reports frame counts, detection counts, P95/P99 processing latency,
original-memory/PTS checks and copy-audit results. `--amd-only` skips DL Streamer
for diagnosis. Without the audit library the report explicitly leaves zero-copy
verification false. `--duration=SECONDS` repeats a 30fps elementary H.264 fixture
at clock pace; SIGINT/SIGTERM requests EOS and drains the pipeline.

```bash
python3 tools/soak.py /path/input.h264 artifacts/yolov8n.mxr /path/yolov8n.json \
  --seconds=3600 --output=artifacts/soak
```

The soak collects RSS, FD and allocated GPU-memory samples, checks bounded growth after warm-up, and
requires DL Streamer integration plus copy-audit success. See
[validation](docs/validation.md) for acceptance thresholds and reproduction.

This PoC covers the analytics path. Encoding, recording, multiple cameras,
RTSP recovery, NPU execution and UI are subsequent development stages.

## Licenses

New code is MIT licensed. External sources remain separately licensed; see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Model and sample-image terms
must be checked separately before redistribution.
