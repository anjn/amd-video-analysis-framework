# Person search in the NVR/VMS demo

The **Search** tab has a resizable left conditions panel and shared result cards
on the right. The initial panel width is 320 pixels. Three modes are available:

- **Image search** opens automatically when a region is dragged on live, paused
  live or recorded video in **Live** single view. The selected crop appears on
  the left and the search starts immediately.
- **Text search** is the initial mode. Searches run after 350 ms without an edit,
  or immediately on Enter. Japanese IME composition is not submitted until committed.
- **Person search** provides checkboxes for age group, skin tone, hair color,
  upper/lower clothing type and color, eyewear and bag type. Each category allows
  at most one checked value; clicking it again removes that condition. Changes
  search immediately. **Clear conditions** removes all selections.

Person conditions generate a visible English description and use the existing
text embedding search. They find similar appearances, not exact attribute
matches or verified ages/skin tones. No attribute classifier or additional
collection inference is used. Conditions are retained separately while switching
modes, but are not persisted across application restarts. Empty text or person
conditions clear the results without submitting a query.

An updated query keeps the preceding results visible until the new result arrives;
changing modes clears them. Superseded requests and thumbnail loads cannot update
newer results. Each card shows the camera, timestamps, cosine similarity, up to
four thumbnails and a selector for every matched image. **Play nearby video**
plays that scene inside the card, with pause and seek controls. Only one card
plays at a time. A result replacement stops playback; leaving Search also cancels
pending searches and debounce timers. Recording deletion invalidates the results.

The **AI Statistics** tab reports completed stage timings and per-camera update
rates. Detection/tracking defaults to 2 Hz per camera; collection requests one
embedding every five seconds per person and slows down under load. See
[AI performance and the 16-camera configuration](nvr-ai-performance.md) for
model selection, bounded queues, priorities and benchmark commands. This is
sampled, best-effort indexing; existing recordings are not backfilled.

## Choosing the embedding model

In **AI Statistics**, choose **EmbeddingGemma 2 (GPU)**, **MobileCLIP2-S2
(GPU FP16)**, **MobileCLIP2-S2 (NPU BF16)** or **MobileCLIP2-S2 (GPU + NPU)**. The selection is saved for the
next application startup; the currently running model and its search results
stay active until then. The desktop's existing startup policy clears old
recordings and their search data. No history is re-embedded on selection.

MobileCLIP2 uses the native API added to `embeddinggemma2-rocm`. Prepare its
bundle using that repository's `docs/mobileclip2.md`, including the backend
profiles to be used. Configure these additional fields alongside the existing
EmbeddingGemma `library` and `bundle`, so both models remain selectable:

```json
"embedding_model": "mobileclip2-s2",
"mobileclip_backend": "gpu",
"mobileclip_library": "/path/to/embeddinggemma2-rocm/build-mc2/libmobileclip2.so",
"mobileclip_bundle": "/path/to/embeddinggemma2-rocm/artifacts/mobileclip2-api",
"ryzen_ai_venv": "/home/amd/work/rai/rai19-venv"
```

`embedding_model` defaults to `embeddinggemma2`; `mobileclip_backend` is `gpu`
or `npu`; `gpu+npu` enables concurrent collection on both. MobileCLIP2 runs in the installed `amdva-mobileclip-worker` process,
which can be overridden with `mobileclip_worker`. NPU workers source
`/opt/xilinx/xrt/setup.sh` and the configured venv's `bin/activate`, isolating
Ryzen AI and its NPU handles from the NVR's HIP initialization. The NPU worker replaces the inherited library path with the ROCm and Ryzen AI
paths so EmbeddingGemma/PyTorch libraries cannot shadow NPU dependencies. GPU
workers inherit the launch environment. The worker uses the native C API, not Python
inference, and shuts down with the parent. A backend error is reported in
Search; there is no silent GPU/CPU fallback.

Both MobileCLIP backends letterbox bounded person/region crops to 256×256 using
the library's preprocessing. GPU accepts batch 1 or 4 (partial collections pad
to 4); NPU uses batch 1. The configured `embedding_batch` remains the GPU
preference. Text uses MobileCLIP's CPU encoder and 77-token tokenizer without
the EmbeddingGemma prompt. EmbeddingGemma retains its portrait profiles.

MobileCLIP returns 512D vectors. They are zero-extended to the existing 768-float
storage envelope, preserving cosine similarity without a catalog migration.
Model manifests, backend and crop limit identify a separate search space;
Each query vector is compared only with records from the matching model and
image backend.
GPU FP16 and NPU BF16 results can differ; Japanese retrieval accuracy also
differs between the model families. ReID features remain independent.
In `gpu+npu` mode, GPU and NPU consume a shared bounded queue independently.
The NPU takes one eligible crop; the GPU takes up to `embedding_batch` (1 or 4).
Neither waits for the other device or for a full batch. Free NPU capacity is
preferred, and a person cannot be in flight on both devices. Every crop is stored
once under the fingerprint of the backend that actually encoded it. Both workers
must initialize successfully; a missing backend is reported rather than silently
turning this into a single-device mode.

**MobileCLIP GPU duty limit** and **MobileCLIP NPU duty limit** in AI Statistics
accept 1–100%, apply immediately and persist as
`search.mobileclip_gpu_budget_percent` / `search.mobileclip_npu_budget_percent`.
Both default to 100% (no intentional post-inference wait). At 20%, each device
waits four times its previous inference duration before its next collection job.
These are scheduling budgets, not hardware utilization caps. GPU work still yields
to YOLO, tracking and user queries; NPU inference does not hold the GPU gate.
The requested per-person interval remains the minimum, without MobileCLIP's
former adaptive 20% pacing. New tracking images are required for subsequent
embeddings; increasing the budget does not recompute an unchanged image.

A region query is encoded on both devices concurrently and each vector searches
only its matching backend's records. Text queries use the shared CPU FP32 text
encoder for both namespaces. Hits are merged by the existing camera/person
rules and ordered by their within-backend cosine scores; vectors from different
backends are never directly compared. Similarity-score distributions can differ
between FP16 and BF16. **Image embedding GPU** and **Image embedding NPU** rows
show device-specific call counts, batch sizes and time; **Image embedding**
aggregates individual completed collection jobs. GPU and NPU jobs can overlap,
so their times must not be summed as elapsed time. Separate GPU/NPU in-flight
rows and the pending queue row show admitted versus waiting work.

SDK consumers must rebuild for the extended `SearchOptions` and floating-point
`Options::ai_fps` / AI frequency interfaces.

## Native runtime setup

Use the EmbeddingGemma 2 ROCm revision pinned in `dependencies.lock.json`:
`351303e293e11bafe4aa09f8303ba8b4d294d128`. Older local binaries may implement a
different profile residency policy even with the same library version.
Build and install that revision using its `docs/build.md`, with ROCm 7.2,
MIGraphX and ROCm LibTorch matching the framework runtime. Python is used for
model preparation; the NVR calls `libeg2` directly through its native C ABI.

After obtaining the model according to its upstream download instructions,
prepare a square image profile and text profiles:

```bash
eg2 prepare --model /path/to/google --output artifacts/nvr-search \
  --budgets 70 --batches 1 --aspect 1 1
eg2 prepare-text --model /path/to/google --output artifacts/nvr-search \
  --batches 1 --lengths 32 128
```

These commands run in the embeddinggemma2-rocm preparation environment. The
output path in the NVR configuration must point to that resulting bundle.
Longer queries require a corresponding prepared text sequence profile; errors
are shown in Search. Model loading and warmup finish before the live inference
scheduler starts, avoiding concurrent HIP graph capture during initialization.

For an installed native runtime, launch with:

```bash
source scripts/env.sh
export EG2_ROOT=/path/to/embeddinggemma2-rocm/install
export EG2_TORCH_ROOT=/path/to/venv/lib/python3.12/site-packages/torch
source examples/nvr/scripts/search-env.sh
build/examples/nvr/amdva-nvr /path/to/config.json
```

The environment helper puts the installed `eg2-hip` aliases before other HIP
libraries so `libamdhip64.so` and its versioned SONAME resolve to the same
runtime. It also enables the experimental AOTriton architecture path required
by gfx1150. Other GPU architectures need their own prepared/validated bundles.

The example NVR config enables search. Configurations omitting `search` retain
the previous behavior. A missing library or unsupported profile disables the
search service with a visible error; reception and recording still start.

```json
"search": {
  "enabled": true,
  "library": "libeg2.so",
  "bundle": "../artifacts/nvr-search",
  "device": 0,
  "image_size": 384,
  "interval_ms": 5000,
  "queue_limit": 128
}
```

Paths containing a directory are relative to the config; a bare library name
uses the dynamic loader search path. `image_size` is 32–1024 and applies to both
axes. Source images retain their aspect ratio and are never enlarged. The
square model input is centered with black padding and uses token budget 70.
The image stored in the DB has no padding. Changing the bundle manifest or
preprocessing selects a separate embedding namespace, preventing incompatible
vectors from being compared.

## Timing, persistence and playback

Person boxes receive a ten-percent margin on each side, clipped to the video.
A dedicated GPU path converts the source to BGRA and bilinearly resizes the
crop to bounded RGB. Only this bounded RGB image is read back for native model
input and PNG encoding. The live and playback rendering paths still use VA,
DMA-BUF and EGL. The copy auditor counts crop readbacks and native embedding
transfers separately; it continues rejecting undeclared transfers in other
threads and paths.

SQLite stores PNG, 768-dimensional normalized FP32 embeddings and source
metadata in one atomic row. The vendored sqlite-vec extension computes cosine
distance over the selected model namespace. This exact scan is intended for
demo-sized histories; it is not an approximate index for unlimited archives.
The query returns the best 500 images and at most 50 groups. Same-camera
neighbors at most ten seconds apart form a group, capped at sixty seconds from
the first hit. Groups are sorted by their best similarity. Similarity is a
ranking score, not a probability or a claim of person identity.

Frame timestamps derive from the same original PTS and receiver wall-clock
anchor used for recording, including separate decode origins and background
keyframes. Images become searchable after the containing recording finalizes.
This introduces approximately one recording-segment delay before recent
observations appear. Search does not repeatedly rerun itself as new segments
finish; submit again to refresh.

Playback includes five seconds before and after the group's hits, limited by
available recordings. Gaps use the existing forward resolution behavior. An
exclusive playback bound prevents frames beyond the requested interval from
being displayed. The player pins only its active and lookahead files. Missing
or corrupt recordings use existing playback notices.

Quota pruning, retention expiry and manual deletion remove associated images
and vectors. In-flight results cannot re-create entries for a deleted interval.
Startup/search reconcile interrupted or externally missing recordings. PNG and
vector payloads count toward managed storage; incremental SQLite vacuum returns
freed pages after deletion. Database migration is additive and enables
incremental vacuum once for older catalogs. Consumers must rebuild against the
updated streaming headers.

A drag pins the displayed live or playback surface from mouse press through crop
completion. Dragging pauses playback; release submits the region and switches
to Search. A click retains the play/pause action, and Esc cancels selection.
Coordinates exclude letterboxing, account for display scaling, and never include
rendered timestamp text. The crop is extracted asynchronously before the
surface can be reused.

## Verification

`search-contract` checks crop dimensions, padding, grouping boundaries, cosine
retrieval, model isolation, image persistence, deletion, pin protection and late
completion. Config tests cover defaults, bounds and persistence. Existing
playback/catalog/renderer tests remain applicable.

For an end-to-end hardware run, serve compatible person-containing videos and
point a dedicated config at the corresponding RTSP URLs and a fresh recording
root. Then run:

```bash
source scripts/env.sh
# Also source examples/nvr/scripts/search-env.sh with the native runtime paths set.
build/examples/nvr/amdva-search-validate /path/to/test-config.json
```

The validator exercises collection, English/Japanese text queries, card
playback and dragging a recorded region. Use `amdva-rtsp-files` for MP4/MKV
fixtures. The demo accepts progressive 8-bit H.264/H.265 inputs up to 2560×1440.
Larger inputs require separate downscaled test copies; original files are not modified.

Add `--exercise-deletion` only with a disposable recording root: it deletes
closed test recordings and verifies removal of their search images. With the
copy-audit library preloaded, validation starts auditing after model warmup and
checks that live/rendering paths introduce no undeclared copies. Set
`QT_SCALE_FACTOR=1.5` to exercise selection at fractional display scaling.

Local hardware validation used `~/work/videos/M01_1440p30_10min_hevc.mp4` and
`M02_1440p30_10min_hevc.mp4` through
separate 90-second 1080p H.264 copies. Both text languages retrieved the purple
skirt visible in those museum scenes. Collection from visible and hidden
cameras, inline playback, recording-region search, cancellation and recording
deletion were exercised. This short integration test does not establish
long-duration throughput or large-archive search performance.

## Person tracking and ReID

Set `search.reid_model` to a compiled OSNet MSMT17 checkpoint to enable
person cards. An empty/omitted path retains the original time-based grouping.
The example configuration uses `../artifacts/reid-model/osnet-x1-msmt17.mxr`.
A configured model that cannot load fails the search service visibly; it does
not silently switch to a different identity algorithm. Reception and recording
continue.

The native fixed-camera tracker implements BoT-SORT-style high/low confidence
association, rectangular Hungarian assignment, timestamp-based center/size
Kalman motion prediction and smoothed OSNet appearance features. It is a local
C++ implementation adapted to sampled inference, not the upstream Python
tracker or its benchmark configuration. Camera-motion compensation is disabled;
PTZ/moving cameras require additional work. When ReID is enabled, the live overlay uses confirmed person-search IDs,
including low-confidence detections that remain associated with those people.
Without ReID, the existing imageless tracker supplies live IDs.

Detection uses confidence 0.1 for association (legacy detection metadata retains
0.25). High-confidence association starts at 0.5, new tracks at 0.6. Two matched
observations confirm a person. Motion association retains tracks for two
seconds; low-score recovery is limited to one second. A lost confirmed track
can reconnect within thirty seconds if its cosine similarity is at least 0.85,
its candidate margin is at least 0.05 in both directions, and displacement is
within three person heights. Reconnection starts a new tracklet while retaining
the person ID. Ambiguous matches create separate identities. These conservative
initial thresholds require site-specific evaluation; similar clothes and heavy
occlusion can still cause mistakes.

Detection and tracking run at `ai_fps` (0.1–10 Hz; default 2) per camera.
Frame timestamps, not assumed frame numbers, drive motion and expiry. The
tracking worker replaces queued frames with the latest frame for that camera;
the semantic worker separately keeps the latest crop for each person. A
confirmed, high-confidence observation is required for semantic collection.
With ReID enabled, stored and ReID crops use the tight person box rather than
the legacy ten-percent margin. ReID applies bilinear resizing to its compiled
input shape, ImageNet RGB normalization and L2-normalized 512-dimensional
features. These features are separate from the 768-dimensional EmbeddingGemma
vectors used for text and region queries. Partial ReID batches repeat the last
valid image and discard padded outputs.

With ReID enabled, hidden cameras retain continuous hardware decoding and
inference instead of five-second keyframe snapshots. Sixteen dedicated hidden
slots support the intended total of sixteen cameras with any visibility mix.
This is a scheduling capacity, not a throughput guarantee for arbitrary input
formats, frame rates or person counts. Without ReID, hidden-camera analysis
retains the legacy keyframe interval.

Search results aggregate by camera and person ID. A card offers a selector for
its matched tracklet intervals, avoiding playback through an entire absence.
Text and arbitrary-region queries use the selected embedding model;
ReID determines which saved images belong to the same person. A region query
is not a guaranteed same-person lookup, especially if it contains multiple
people. IDs do not cross cameras, receiver recording sessions or application
restarts. The database migration adds person/tracklet IDs and ReID features;
existing observations remain intact. The embedding namespace includes the ReID
model bytes and preprocessing version, so incompatible histories cannot mix.
Old time-grouped history is available when using its original configuration.

### Prepare and verify OSNet

Download the source revision and weight URL in `dependencies.lock.json`'s
`person_reid` entry and verify the listed SHA-256 values. The weights must be
MSMT17 ReID-trained, not the ImageNet-only checkpoint. Model preparation uses
the existing `.deps/model-export` Python environment; production inference is
native C++/MIGraphX and does not import Python.

```bash
source scripts/env.sh
export PATH="$PWD/build/src/gst:$PATH"
.deps/model-export/bin/python examples/nvr/scripts/export_reid_model.py \
  --source /path/to/torchreid/models/osnet.py \
  --weights /path/to/osnet-msmt17.pth --output artifacts/reid-model
build/examples/nvr/amdva-reid-validate \
  artifacts/reid-model/osnet-x1-msmt17.mxr \
  artifacts/reid-model/reference.rgb artifacts/reid-model/reference.f32
```

The export script rejects incompatible non-classifier parameters and records
hashes and preprocessing. The native validator compares the normalized feature
against PyTorch's FP32 reference (cosine >= 0.999) and reports latency. On this
gfx1150 test host, FP16 inference produced cosine 0.99972, median 2.05 ms and
p95 2.32 ms for a single prepared crop. This excludes video cropping, YOLO and
EmbeddingGemma. The search contract tests cover detection order, low-score
recovery, tentative tracks, short reappearance, ambiguity, stale timestamps,
expiry, identity grouping and persistence. M01/M02 integration also exercises
hidden-camera collection, fractional display scaling, playback, region search,
copy audit and deletion. These checks do not establish labeled-dataset IDF1 or
cross-camera identity accuracy.


Live person overlays use three logical-pixel colored borders with a black halo,
fixed 16-pixel vector text, and a black label panel at 45% opacity. Both border and text use a
stable hue derived from the person ID; labels show only the ID and
current detection confidence (for example, `#2 23%`). Confirmed low-score associations (0.1–0.5) remain
visible. Low-score detections alone do not create new people. Tracking snapshots
expire after one second and are rejected across decoder generations or changed
image dimensions. The validation tool can capture this overlay with
`AMDVA_VALIDATE_CAPTURE=1`; framebuffer readback is only used by that optional
validation capture, never by the normal live renderer.

## MobileCLIP integration checks (2026-10-24)

The native worker passed GPU and NPU image batches 1/3/4 (including padding or
splitting), English/Japanese text calls and normalized 512D output checks.
With two 1440p RTSP inputs, all three embedding selections passed live collection,
text queries, inline recording playback, live/paused-live/recorded region search
and deletion. This checks functionality, not retrieval accuracy or 16-camera throughput.
The UI validation exercised 1.3 Hz / 0.7 s targets, persisted all three model
selections and restored the initial model without changing the running encoder.
Local logs are in `artifacts/mobileclip-switch/`.

Before independent device dispatch was introduced, the combined mode passed a
two-camera 1440p run with both backend
namespaces populated (224 GPU / 81 NPU observations in the collection check),
four-way UI selection persistence, and the same text/region/deletion workflow.
The 40-second statistics run measured mean GPU calls of 25.24 ms, NPU calls of
13.63 ms and combined collection wall time of 25.26 ms, with logical batches
averaging 3.59 images. Device rows include only work sent to that device and
should not be summed as serial latency. These are short functional checks,
not a throughput benchmark. Logs are under `artifacts/mobileclip-combined/`.


## Independent MobileCLIP scheduling checks (2026-10-25)

A 16-source 1440p test used 2 Hz tracking, a deliberately short 100 ms requested
person interval and GPU/NPU budgets of 100%. Across approximately 90 seconds
with 16, four and zero visible cameras, sampled embedding and tracking queue drop
counters stayed at zero. The maximum sampled embedding queue depth was 51
of 128. The final sample reported 4,838 stored observations and no search error;
both backend namespaces were populated. The final rolling window measured mean
GPU calls of 29.74 ms (batch 4) and NPU calls of 17.86 ms (batch 1).
These figures describe this short run, not a guarantee for every input workload.
The demo's configured person interval was not changed.

UI validation applied and persisted GPU/NPU budgets of 35%/80%, then restored
both original settings. A separate two-camera run passed English/Japanese
text queries, inline playback, live/paused-live/recorded region searches and
deletion. All 33 CTests passed; the scheduler, configuration and search contract
tests also passed after the final initialization change. Local logs are under
`artifacts/mobileclip-budget/`.

The NPU-only mode was also checked with 16 sources and the unchanged 5-second
person interval for 40 seconds: 487 stored observations, maximum sampled
embedding queue depth 65/128, and zero embedding/tracking queue drops.

## Search modes validation (2026-10-25)

All 34 CTests passed, including a new UI-only test for debounce replacement,
Enter submission, Japanese IME commit, empty conditions, mode switching,
exclusive/unset checkboxes, generated descriptions and cancellation. The search
contract tests were rerun after the final layout adjustments. A two-camera
1440p MobileCLIP GPU+NPU run passed checkbox person search, incremental
English/Japanese text queries, result playback, recorded/live/paused-live region
search and deletion. The purple-skirt query returned matching clothing in the
reviewed top results; this is a functional spot check, not an attribute accuracy
benchmark. Logs and the reviewed screenshot are in `artifacts/search-modes/`.
