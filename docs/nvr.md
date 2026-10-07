# Desktop NVR sample

`amdva-nvr CONFIG.json` records every registered RTSP source and displays up to
16 selected sources in a Qt desktop window. YOLOv8n runs on the selected sources
with a shared MIGraphX batch executor, optionally also analyzing keyframes from
hidden sources. Source registration has no fixed count
limit; camera, storage, network and decoder capacity still limit a deployment.
The initial validated format is progressive, even-sized, 8-bit NV12 up to
1920×1080, BT.709 limited range, from H.264 or H.265 at 30 FPS. Audio is ignored.

## Build and start

Run `python3 scripts/bootstrap.py` to build the locked dependencies, including
Qt 6, SQLite, RTSP/RTP and Matroska plugins. The sample is enabled by default;
`-DAMDVA_BUILD_NVR=OFF` builds the plugins and headless streaming service without Qt. Dependencies
remain under `.deps`; no system packages are installed or replaced.

Prepare the same YOLOv8n COCO weights in a separate CPU-only export environment:

```bash
source scripts/env.sh
python3 scripts/setup_model_export.py
export PATH="$AMDVA_ROOT/build/src/gst:$PATH"
.deps/model-export/bin/python scripts/export_nvr_model.py \
  /path/to/yolov8n.pt artifacts/nvr-model --batch=4
cp examples/nvr.json artifacts/nvr-local.json
# Edit paths and camera URLs in the copied config; paths are relative to that config.
WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland-egl \
  build/src/nvr/amdva-nvr artifacts/nvr-local.json
```

Alternatively run under an X11 desktop with `QT_QPA_PLATFORM=xcb`; the environment
selects the EGL integration. An AMD hardware OpenGL ES 3 context and DMA-BUF
import are required. Software rendering is rejected. No CPU image upload fallback
is provided.

Add a source using **Add RTSP**, remove the selected table row using **Remove
selected row**, and use **View** to enable or disable decoding, display and AI.
A seventeenth view selection is rejected without replacing an existing view.
Recordings continue when a source is not selected. The table shows connection,
recording state, processed display FPS, AI FPS, AI age and recording gaps.
Configuration changes are saved atomically with owner-only permissions. The
application's status messages omit URLs that may contain credentials.

The config keys are demonstrated in `examples/nvr.json`. Batch size is one of
1, 2, 4, 8 or 16; `model` must match that size and `single_model` must be a
batch-1 model for partial batches of one. Model-info describes one image. Schema-1 models remain readable; new models use the general schema-2
GPU/hash/device-I/O manifest and record the batch size. Dynamic-batch ONNX is exported from the
weights; changing a fixed-batch ONNX input alone is insufficient.

## Shared services, tracking and publication

The controller, recording catalog, keyframe selector and config persistence now
live in `amdva_streaming` and can be used without Qt. GPU image packing, batch
execution, imageless tracking, overlay geometry and result publication use shared
framework libraries. See [framework interfaces](framework.md) for SDK and
headless usage. The original `amdva::nvr` controller/catalog/keyframe headers
remain source compatibility wrappers for the desktop sample.

`tracking_enabled` defaults to true. Selected channels run imageless DL Streamer
tracking across decoded frames and draw class labels and tracking IDs. A source
reconnect, selection change or incompatible caps resets its tracker. Hidden
keyframe detections remain independent untracked results.

Optional `results` configuration uses the same asynchronous publisher as
`amdmetapublish`:

```json
"results": {
  "method": "file", "file_path": "events.jsonl",
  "address": "", "topic": "amdva/analytics",
  "queue_limit": 256, "byte_limit": 4194304
}
```

Use `method=none` to disable output (default), `mqtt` with an address such as
`tcp://127.0.0.1:1883`, or `kafka` with `127.0.0.1:9092`. JSON contains selected
tracked observations and background observations with source generation and
PTS. File paths are relative to the config. Full queues discard oldest waiting
records; delivery errors do not stop recording/display. The UI reports delivered,
dropped and error counts. Output is best effort and is not a durable event log.

## Background AI for hidden sources

Set `background_ai_enabled` to `true` to analyze every source whose **View**
checkbox is unchecked. `background_ai_interval_seconds` is an integer from 1
through 3600 and defaults to 5. An omitted enable flag defaults to false for
existing configurations; `examples/nvr.json` enables the feature. Selected
sources continue using their normal display inference scheduler.

Each hidden source takes the next independently decodable access unit after its
sampling deadline. H.264 accepts IDR pictures; H.265 accepts IDR, BLA and CRA
pictures. The compressed parser caches parameter sets by ID, capped at 1 MiB
per source, and validates slice-header references before admission. An ordinary
I picture or the absence of `DELTA_UNIT` alone is insufficient. Corrupted units,
H.264 extensions and multilayer H.265 are excluded. The existing progressive,
8-bit NV12, size and colorimetry restrictions still apply.

A fresh VA decoder receives cached parameters and exactly one selected AU, then
EOS to drain reordered output. No dependent pictures are decoded. Decode timeout
is two seconds; errors affect background AI only. There is no full-stream or CPU
decode fallback. Recording always receives the original complete compressed
stream. Compressed headers may be copied on the CPU; decoded pixels remain in
VAMemory through VPE, HIP and MIGraphX.

Four background slots bound the entire decode-to-inference workload. Each owns
only a reusable AI surface and mapping, with no preview/EGL resources. When all
slots are occupied, a candidate is skipped rather than queued. Older sampling
deadlines have priority, with rotating ties. The five-second deadline starts at
admission; long GOPs or contention increase the actual interval. Sampling does
not request extra keyframes from cameras.

Background and displayed images share the existing batch executor. A mixed
batch admits at most one background image, and background-only batches may fill
all available batch positions. Partial batches retain the existing batch-1 and
GPU-padding behavior. Selection, removal and reconnection cancel pending
background jobs; generation checks reject obsolete completions. Background
boxes are never reused as display overlays.

The status column reports background mode, completed inference count, detection
count and result age. `Controller::status()` and the validation JSON telemetry
also expose decoded/attempt counts, active jobs, capture timestamps and the
latest class/score/box coordinates. Results are retained in memory only; this
feature does not create a persistent detection-event database.

## Pipelines and failure handling

Each source has three independently controlled pipelines:

```text
rtspsrc → depay → parse → compressed appsink
                       ├─ buffer references → recorder appsrc → parse → splitmuxsink
                       └─ selected only → decoder appsrc → VA decoder → VAMemory appsink
                                                                  ├─ VPE → EGL tiles
                                                                  └─ VPE → HIP → batch MIGraphX → GPU NMS
```

Compressed queues are limited to 16 MiB and approximately two seconds per
receiver. Appsrc queues are also bounded. No source callback waits for disk I/O,
AI or the GUI. A compressed backlog restarts the affected recording or decoder
at a keyframe instead of dropping dependent pictures silently. Recording errors
and quota exhaustion are visible; reception and selected display continue.

RTSP uses TCP, a 100 ms jitter latency and a five-second no-video watchdog.
Reconnect delays grow from one to thirty seconds with jitter and repeat until
the source is removed. Every reconnect gets a new generation and recording
session. Decoder format changes are renegotiated; unsupported formats produce a
visible decoder error. Old-generation detections are rejected. Resolution
changes invalidate overlays, and results older than 500 ms are hidden.

Recording is the original encoded video, with parser framing converted as
required by Matroska. Files split at keyframes around `segment_seconds`, so a
long GOP can make a segment longer than the configured target. Filenames contain
UTC epoch microseconds, channel ID, session UUID and fragment number. Window close, SIGINT and SIGTERM perform graceful shutdown. Shutdown
sends EOS and allows up to five seconds for asynchronous muxers to finish.

SQLite catalogues files created by this application. The recorder root has an
exclusive writer lock. Once the managed files exceed `quota_gib` or available
space drops below `reserve_gib`, the oldest closed managed files are deleted.
Active recordings, unregistered files and symlinks are excluded. If no eligible
files can restore capacity, recording pauses and resumes at a keyframe when
space becomes available. After an abrupt shutdown, previous active entries
become deletion candidates; they are not claimed to be valid finalized MKVs.

## GPU ownership and scheduling

Startup requires an observed increase of the dedicated VPE engine counter.
The application keeps one shared GstVaDisplay and 16 persistent view slots.
Preview and AI use separate VA processing contexts. The preview context is renewed
when source or output geometry changes; AI does the same on source geometry changes.
Preview downscaling is limited to the validated 6:1 range, with further window
reduction performed by GL sampling. A minimized window therefore does not submit
extreme reduction ratios to VPE.

Each slot owns a 640×640 linear BGRA VPE output with a persistent HIP mapping,
plus three 1920×1080 linear BGRA preview surfaces imported into EGL once.
Only the occupied rectangle is sampled; tile size changes do not reimport GPU
allocations. GL fences protect preview surfaces from reuse while rendering.
Selection changes reuse these pools, including across camera reconnects.

Preview conversion runs separately from inference. Each source keeps only its
latest decoded buffer. HIP packs RGB NCHW directly into the corresponding slice
of a reusable device batch arena. There is no per-frame tensor concatenation or
H2D/D2D tensor copy. The scheduler rotates across eligible sources, waits at most
20 ms for a batch, uses batch 1 for a single image, and fills unused slots of a
larger batch with a HIP kernel. Only valid slots produce results. Postprocessing
is performed independently per image on the GPU. Allocator layout probes may
derive a newly-created, never-rendered test surface to discover pitches; they do not map or transfer frame pixels. The copy auditor
counts these separately and still rejects derivation of any rendered surface.
Each valid image reads back a 7204-byte compact detection structure; frame pixels remain on the GPU.

AI starts at 5 FPS per selected channel. Measured batch service time and frame
age adjust sampling, up to 30 FPS per channel, with a 200 ms age target. These
are scheduling targets, not throughput or maximum-latency guarantees. Each
result retains channel ID, a globally increasing generation and its geometry.
Inference does not retain a growing frame queue. The original `amdpreproc` and
`amdmigraphx` elements remain usable and share the same inference implementation.

## Verification

`ctest --test-dir build --output-on-failure` includes the existing hardware and
metadata tests and catalog tests for ownership, quota, active protection,
symlinks, path traversal and concurrent-writer exclusion.

`amdva-batch-reference` and `tools/compare_batch_reference.py` compare distinct
images and valid batch occupancies 1–4 against CPU FP32 ONNX. Fixture upload and
framebuffer capture are explicitly offline test operations, outside the live
video processing path.

`amdva-nvr-rtsp-server H264_FILE H265_FILE [PORT]` serves 32 looped local camera
paths, alternating H.264/H.265. Use pre-encoded 1080p30 files with a starting
keyframe and no B frames; the test server does not encode or decode video.

For a 20-source configuration with the first 16 selected:

```bash
source scripts/env.sh
python3 tools/nvr_soak.py /path/to/20-source-config.json \
  /path/to/1080p30.h264 /path/to/1080p30.h265 \
  --seconds=260 --cycles=100 --restart-at=100 \
  --output=artifacts/nvr-dynamic-validation
python3 tools/nvr_soak.py /path/to/20-source-config.json \
  /path/to/1080p30.h264 /path/to/1080p30.h265 \
  --seconds=3600 --restart-at=600 --output=artifacts/nvr-soak
```

The harness verifies the view limit, repeated add/remove/select/unselect,
recovery after server restart, live copy audit, recording finalization and MKV
metadata. It captures an actual EGL-rendered tile image and reports per-channel
rates and GPU batch timing. Resource monitoring covers RSS, deduplicated DRM
allocation counters and FDs, with a 64 MiB growth guard for RSS/GPU allocation
and a bounded allowance for asynchronous recording finalizers. Telemetry also
reports live and free glibc heap bytes. NVR entry points initialize GStreamer with
a default limit of four glibc heap arenas, before worker creation; an explicit
`MALLOC_ARENA_MAX` environment setting takes precedence. Native callers should
call `initialize_runtime()` before creating Qt or GStreamer workers. After
pipelines are retired or reconnected, the controller
waits for frame owners to drain and trims free heap pages, with a five-second
cooldown, to limit RSS retained by transient GStreamer worker arenas. It aborts before
unbounded growth can exhaust host memory. Recorded validation results are in
`docs/nvr-validation-results.json`.

On Ryzen AI 9 HX 370 / Radeon 890M, the one-hour local RTSP test passed with
20 recording sources and 16 selected views, 100 dynamic source/selection cycles,
and a server restart that swapped H.264/H.265 at the same URLs. All sources
recovered in seven seconds. Median per-view display and AI rates after recovery
were 29.88 and 5.98 FPS. All 1,304 finalized MKVs passed metadata validation;
60 frames from the latest recording of each of the 20 channels also decoded
successfully. Live copy audit reported zero violations. After warm-up, maximum
RSS and GPU allocation growth were 11,200 and 7,124 KiB, with no FD growth.

`amdva-nvr-faults CONFIG.json` uses the first source to exercise failed RTSP
registration, credential masking, 64×64 window resizing, recording-directory
permission failure and recovery. Use a dedicated recording directory; this test
temporarily changes its permissions.

This sample has no playback, ONVIF discovery, audio recording or 4K performance
claim. A production NVR should additionally define crash repair, retention
policy, monitoring, access control and camera-specific qualification.

The optional `nvr-keyframe-tests H264|H265 FILE OUTPUT_PREFIX` executable checks
single-AU VA decoding and writes extracted AUs for offline reference work.
Use B-frame/open-GOP fixtures as well as closed-GOP fixtures to distinguish
ordinary intra pictures from valid random-access pictures. Extracted AUs and
`amdva-dump-tensor` readbacks are offline test artifacts, not live processing.
`tools/compare_background_reference.py` compares live background boxes with CPU
FP32 ONNX detections from matching offline keyframe tensors.

The background reference comparator checks class/count agreement, IoU >= 0.95
and score delta <= 0.03. With `--allow-fp16-nms-ties`, it also accepts a
CPU-suppressed overlapping candidate whose score is within 0.005 of the CPU
winner, retaining those geometric/confidence checks. This addresses measured
FP16 ranking changes rather than lowering the IoU threshold; the report records
both the canonical post-NMS IoU and the number of alternate winners. On the
second bus IDR, one low-confidence box differs from the FP32 NMS winner (IoU
about 0.93), while matching the alternate CPU candidate above 0.994. The existing
offline GPU executor reproduces the background result on that same tensor.

Background AI completed the one-hour 20-source / 16-view test with 100 dynamic
cycles and codec-swapping reconnection, recovering receive/record/display in
7.5 seconds. All 1,309 MKVs passed metadata checks, and 60 frames from the latest
recording of every channel decoded successfully. Copy-audit violations were
zero; maximum RSS/GPU growth was 22.3/21.2 MiB, with at most four background jobs.
Sequential 70-second runs with background AI enabled/disabled measured display
medians of 29.86/29.90 FPS and selected AI medians of 5.98/5.98 FPS. These are
local-fixture observations. Full results and reference-comparison conditions
are in [background AI validation results](nvr-background-validation-results.json).
