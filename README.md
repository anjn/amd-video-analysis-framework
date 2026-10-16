# AMD Video Analysis Framework

GStreamer video analytics for AMD APUs, using VA codecs/VPE, HIP image
processing and MIGraphX inference. Selected Intel DL Streamer elements provide
tracking, stream mux/demux, metadata aggregation and interoperability.

```text
RTSP / file → VA decode → amddetect → gvatrack → amdclassify (ROI)
                       → amdpython → amdmetapublish → amdwatermark → DMA-BUF sink
```

The framework supports YOLOv8 detection, instance segmentation and pose,
classification logits, semantic segmentation and generic GPU tensor results.
Image and mask processing stay on the GPU; only bounded result metadata returns
to the CPU. Shared batches support static sizes 1/2/4/8/16 and partial timeouts.

The reusable `amdva_streaming` library provides dynamic RTSP registration,
recording, reconnect, selected-source analytics and background keyframe AI
without Qt. The desktop NVR uses that library to display up to 16 tiles with
tracked detections. A headless command and installed CMake SDK are included.

See [framework support and examples](docs/framework.md),
[memory contracts](docs/architecture.md), [NVR usage](docs/nvr.md), and
[validation](docs/validation.md), and [decode/presentation telemetry](docs/telemetry.md). AMD elements use their own model-info format;
they do not implement every upstream DL Streamer model adapter or its gva
inference backend. The legacy `amdpreproc ! amdmigraphx` pipeline remains
available.

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
v2026.2.0, ROCm 7.2.0 / MIGraphX 2.15.0, and OpenVINO 2026.2.0, with Paho MQTT, librdkafka and embedded CPython. The selected
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

Video recording preserves compressed RTSP packets; the framework does not
provide an NPU inference backend. The [resource dashboard sample](docs/resource-dashboard.md)
uses hardware H.264 encoding for GPU-rendered monitoring graphics.

## Licenses

New code is MIT licensed. External sources remain separately licensed; see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Model and sample-image terms
must be checked separately before redistribution.

## Desktop NVR sample

The Qt sample receives and records dynamically registered RTSP cameras, displays
up to 16 selected H.264/H.265 channels through VA/VPE/EGL, and overlays YOLOv8n
GPU batch detections. It supports automatic reconnect and quota-based deletion
of closed managed MKV recordings. See [NVR setup, architecture and verification](docs/nvr.md)
and [the example configuration](examples/nvr.json). Optional background AI
decodes only independently decodable keyframes from hidden sources, at a
configurable interval (five seconds by default), using four bounded AI slots.

The desktop sample includes fixed live layouts, camera management, recording search and hardware playback; see the [VMS GUI guide](docs/vms.md).

The [resource dashboard prototype](examples/resource-dashboard/README.md) is a
standalone HTML preview of CPU, GPU and DRAM monitoring, with simulated readings
and a 1080p video composition. The [native RTSP sample](docs/resource-dashboard.md)
collects real metrics and serves the composition through GPU rendering and
hardware H.264 encoding.

For video files as repeatable RTSP camera inputs, see the
[file-to-RTSP tool](docs/rtsp-files.md).
