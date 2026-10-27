# Validation and reproduction

The original single-stream results remain in `validation-results.json`; NVR
regressions have separate reports. The expanded framework's task, publication,
Python, batching, GPU drawing and SDK checks are described in
[framework validation](framework-validation.md) and its
[recorded results](framework-validation-results.json). Logs, compiled models and video
fixtures remain in ignored `artifacts/`. Reports record completed checks on the
declared host, rather than compatibility claims for every APU/model.

## Hardware and zero-copy evidence

`amdva-probe` creates known NV12 test fixtures, scales/converts them with VA, and
exports a linear BGRA allocation. HIP samples it, then samples the **same mapped
pointer** after VPE rewrites that allocation with a different fixture. Both
checksums must match, and the process VPE counter must increase. CPU fixture
uploads and scalar verification downloads are intentional probe operations.
They do not appear in the analytics video pipeline.

`amdva-run` checks original GstMemory identity and PTS at the AMD input and final
output. The preload auditor separately intercepts HIP memcpy operations, VA
image download/upload/derive calls and VA image memory mapping. It permits the legacy 7204-byte D2H detection record, explicitly marked
bounded metadata transfers, and GPU-to-GPU assembly/ownership copies. Unmarked
host transfers and decoded-image mapping are rejected. The compiled MIGraphX graph is also checked for copy-to/from-GPU
operations. This evidence covers the tested public API path; it does not claim
to trace every internal driver operation or establish compatibility on other
APUs. Importer/driver changes require repeating the alias and pipeline probes.

The reported P95/P99 measure time from `amdpreproc` input to final output sink
pad, including inference and DL Streamer elements. They exclude source/decode
latency and sink presentation wait. Overall FPS includes initialization. A
short file's FPS therefore does not represent steady throughput.

## Correctness and failure tests

```bash
source scripts/env.sh
cmake -S . -B build -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT" \
  -DAMDVA_HARDWARE_TESTS=ON -DAMDVA_DLSTREAMER_TESTS=ON
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

The original baseline tests cover layout bounds/overlap/modifiers, tensor ownership and shallow
copying, PTS and original memory identity, strict caps, missing-model/non-strict
initialization rejection, hardware aliasing/VPE execution, real preprocessing
against independent BT.709 RGB values and letterbox boundaries, and 10000
import/map/free cycles on one allocation. The preprocessing reference sends two
different fixtures through the same intermediate surface and verifies that a
retained first tensor is not overwritten. A 3000-frame metadata-only DL Streamer
stress test requires all six detections per frame and peak RSS growth <= 4 MiB
after 500 warm-up frames. It exposes partial string-getter failure leaks in JSON
classification conversion. The corrected implementation grew 8 KiB in the local
check; heap profiling found no remaining leaked classification-label call stack.
Hardware tests are opt-in. The DL Streamer test is enabled by bootstrap.

For model accuracy, use a separately downloaded CPU ONNX Runtime 1.24.4 and the
same input tensor. The dump tool deliberately downloads that tensor for offline
validation; it is outside the audited pipeline.

```bash
uv pip install --python .deps/venv/bin/python onnxruntime==1.24.4
build/src/gst/amdva-dump-tensor artifacts/bus.h264 artifacts/input.f32 \
  > artifacts/input-geometry.json
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/src/gst/amdva-run artifacts/bus.h264 artifacts/yolov8n.mxr \
  /path/yolov8n.json --events=artifacts/events.jsonl
.deps/venv/bin/python tools/compare_reference.py /path/yolov8n.onnx \
  artifacts/input.f32 artifacts/input-geometry.json artifacts/events.jsonl \
  /path/coco80.txt
```

The reference performs FP32 CPU inference, independent class-aware NMS and
coordinate restoration. Each detection must have the same class, IoU >= 0.90
and confidence difference <= 0.05, with no unmatched GPU detections. The tested
frame had six detections; all matched (minimum IoU 0.9928, maximum confidence
difference 0.00309). This is a reproducible correctness check, not a dataset-wide
accuracy evaluation. A separate synthetic test validates preprocessing.

DL Streamer's JSON-lines publisher appends to existing files. Use a fresh events
path for each run; the reference checker rejects ambiguous duplicate timestamps.
The soak tool rejects an output directory that already contains events.

Model mismatches, runtime errors and unsupported caps fail through the GStreamer
bus. EOS, timed loop drain, repeated process startup and clean shutdown are
exercised by the run/soak checks. Flush behavior follows GstBaseTransform, but
active-flush fault injection and GPU-hang recovery have not been validated.

## Long operation

The one-hour soak runs a 1080p30 H.264 fixture on a clock-paced loop through both
AMD and DL Streamer elements. It samples RSS, FD count and allocated VRAM/GTT
every five seconds. GPU allocation counters are aggregated once per DRM client
from fdinfo; duplicate FD aliases are excluded, and missing counters fail validation.
After a 60-second warm-up, peak FD growth must be <= 4 and peak RSS growth
<= 64 MiB relative to the baseline sample. Peak GPU allocation growth must also
be <= 64 MiB. Excessive growth stops the test before exhausting host memory.
It also requires successful EOS drain,
equal input/output counts, original memory/PTS identity, and copy-audit success.
The limits are regression thresholds for this PoC, not general SDK memory budgets.

The completed local one-hour run processed 107970 input/output frames and
647820 detections, with zero identity or observed copy violations. Overall FPS
was 29.9876, P95 processing latency 10.412 ms and P99 10.4895 ms. After warm-up,
peak RSS growth was 696 KiB and peak FD growth was 2. Allocated GPU memory ranged
from 302540 to 308748 KiB; its peak did not exceed the baseline sample. EOS drained
successfully. Full results and environment are in
[validation-results.json](validation-results.json).

```bash
python3 tools/soak.py artifacts/bus.h264 artifacts/yolov8n.mxr \
  /path/yolov8n.json --seconds=3600 --output=artifacts/soak
```

Four issues were found and corrected during validation: VA export FDs were not
consumed by the tested AMD HIP importer, and device-I/O MIGraphX execution needed
explicit completion before default-stream GPU postprocessing. In addition,
per-frame new-surface imports retained driver GPU allocations; keeping one
intermediate VA surface and HIP mapping bounded that allocation. FD stress,
two-frame preprocessing, first-frame CPU reference and allocated GPU-memory
monitoring are retained as regression checks. RSS/FD monitoring alone missed
the GPU allocation growth. DL Streamer JSON classification conversion leaked
labels when a subsequent model-name lookup failed; borrowing both strings fixes
the partial-failure and exception paths. A metadata-only CPU stress test covers
that integration regression.

## Fixture used locally

The validation uses the existing background project's YOLOv8n ONNX/metadata and
the Ultralytics bus image, encoded with FFmpeg/libx264 to 1920x1080 H.264,
30fps, limited BT.709. The source image is stretched to 1920x1080 for the fixture;
preprocessing then letterboxes it to 640x640. No model/image is redistributed
in repository sources. A synthetic H.264 test pattern also exercises the path.

```bash
curl -fsSL https://raw.githubusercontent.com/ultralytics/ultralytics/main/ultralytics/assets/bus.jpg \
  -o artifacts/bus.jpg
ffmpeg -hide_banner -loglevel error -loop 1 -i artifacts/bus.jpg \
  -vf 'scale=1920:1080:out_color_matrix=bt709:out_range=limited' -t 1 -r 30 \
  -c:v libx264 -pix_fmt yuv420p -color_primaries bt709 -color_trc bt709 \
  -colorspace bt709 -f h264 artifacts/bus.h264
```

For diagnosis collect `GST_DEBUG=amdpreproc:6,amdmigraphx:6,gvatrack:5`, dependency
lock/cache verification, model manifest, probe JSON, runner JSON and soak CSV.
Pinned userspace alone does not pin host Mesa, firmware or the kernel.
