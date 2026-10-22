# Person search in the NVR/VMS demo

The **Search** tab finds recorded person crops using a text description or a
region dragged on the **Recordings** video. Both modes use the same result
cards. Each card shows the camera, timestamps, cosine similarity, up to four
thumbnails and a selector for every matched image. **Play nearby video** plays
that scene inside the card, with pause and seek controls. Only one card plays
at a time; leaving Search releases playback.

The initial collection policy samples at most one detected frame per second
from each visible camera. Hidden cameras use the existing background keyframe
AI interval (five seconds by default). Enabling search also enables background
AI. Every person detected in an admitted frame is a candidate. Work is bounded
by 64 pending crops and four source frames, including the frame being processed;
overload skips new collection and reports a counter. Search requests take
priority between crops. This is sampled, best-effort indexing, not exhaustive
tracking or person identification. Existing recordings are not backfilled.

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
  "interval_ms": 1000,
  "queue_limit": 64
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
fixtures. Inputs above the demo's 1920×1080 limit should be converted to separate
1080p test copies; original files are not modified.

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
