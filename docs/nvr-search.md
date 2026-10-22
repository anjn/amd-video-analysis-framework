# Person search in the NVR/VMS demo

The **Search** tab finds recorded person crops using a text description or a
region dragged on the **Recordings** video. Both modes use the same result
cards. Each card shows the camera, timestamps, cosine similarity, up to four
thumbnails and a selector for every matched image. **Play nearby video** plays
that scene inside the card, with pause and seek controls. Only one card plays
at a time; leaving Search releases playback.

The **AI Statistics** tab reports completed stage timings and per-camera update
rates. Detection/tracking defaults to 2 Hz per camera; collection requests one
embedding every five seconds per person and slows down under load. See
[AI performance and the 16-camera configuration](nvr-ai-performance.md) for
model selection, bounded queues, priorities and benchmark commands. This is
sampled, best-effort indexing; existing recordings are not backfilled.

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
source scripts/search-env.sh
build/src/nvr/amdva-nvr /path/to/config.json
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

A drag pins the displayed playback surface from mouse press through crop
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
# Also source scripts/search-env.sh with the native runtime paths set.
build/src/nvr/amdva-search-validate /path/to/test-config.json
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

Detection and tracking run at `ai_fps` (1, 2 or 5; default 2) per camera.
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
Text and arbitrary-region queries continue to use EmbeddingGemma similarity;
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
.deps/model-export/bin/python scripts/export_reid_model.py \
  --source /path/to/torchreid/models/osnet.py \
  --weights /path/to/osnet-msmt17.pth --output artifacts/reid-model
build/src/streaming/amdva-reid-validate \
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
