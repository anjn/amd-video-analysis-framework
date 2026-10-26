# Desktop video management

The NVR desktop now has **Live**, **Cameras**, **Search** and **AI Statistics** pages. It uses
Qt Widgets and the existing hardware EGL renderer. RTSP reception, recording,
reconnection, selected-camera analytics and optional background analytics are
services of the Qt-free NVR backend (`backend/`), built on `amdva_streaming`.

## Live monitoring

Choose 1, 4, 9 or 16 views. A tile is selected immediately on mouse press,
without waiting for double-click detection. Click a tile, then choose its camera in the toolbar,
or double-click a camera in the left list. **Clear tile** removes that mapping.
Assigning a camera already used elsewhere exchanges the two tiles. Empty and
disconnected tiles keep their position. Camera titles, connection states and the
selected border remain visible over the grid.

**Show stream info** toggles codec, received resolution, input frame rate and
bitrate on every live image. It defaults to on and persists in `<config>.ui.json`.
The lower-left overlay uses one line, for example
`H.264 1920x1080 30.0 fps 4.2 Mbps  decode 12.3 ms`.
Decode latency is the existing rolling mean from application input to decode output,
not camera capture latency; unavailable telemetry displays `decode N/A`. Grid
text uses a 14px target size, while single view uses 22px. Rates update approximately once per second from received
parser-output compressed access units before queue drops, decoding and recording
selection.
Bitrate is encoded video payload in decimal Mbit/s, displayed to one decimal;
it excludes RTP/RTSP/TCP/IP overhead and audio. Frame rate is measured input
rate rather than decoder/display FPS or a nominal SDP value. Codec/resolution
come from parser caps and reset on reconnection. GPU geometry renders these
labels without reading video pixels or uploading text images. DejaVu Sans
semibold outlines are tessellated and antialiased with 4× MSAA. Text scales to
fit the tile width; the black backing has 45% opacity so the video remains
visible through it. Playback timestamps use the same readable font and backing.

`Controller::status()` exposes `input_codec`, `input_width`, `input_height`,
`input_fps` and `input_mbps` for headless consumers as well. Rebuild SDK clients
after the public `Status` structure change.

Double-click a tile, or use **Single / grid**, to focus it. Double-click again or
press Esc to return. Focusing does not change the original grid's decode/AI
selection. Reducing the grid does deselect cameras outside the visible cells,
but retains their assignments for restoration. Unselected cameras follow the
configured background-AI policy. Every registered camera continues recording.

Mappings use stable source IDs, independent of decoder slot allocation. The
application stores them in `<config>.ui.json` with owner-only permissions and
atomic replacement. Existing configs initialize mappings from selected cameras;
a fresh unselected configuration starts with four empty tiles. The media config
and the UI sidecar have separate ownership; headless users need no UI fields.
There is one saved arrangement, rather than multiple named views.

## Camera management and storage

Each `amdva-nvr CONFIG.json` launch deletes the previous catalogued recordings
in that configuration's recording root, together with their search images,
embeddings and saved analysis, before starting camera reception. Interrupted
recordings from a previous run are included. The existing exclusive catalog lock
prevents clearing a root used by another process; deletion failures abort startup.
The catalog database and unrelated files are retained. This startup policy belongs
to the desktop executable; SDK catalog/controller users do not clear history automatically.

**Cameras** provides add, edit and remove actions. Name changes preserve the
connection and source ID. URL changes retire only that camera's current receive,
decode and recording session, then reconnect using the new URL. Removing a
camera stops its reception and clears its mappings, without deleting history.

The table retains connection, recording, analytics and telemetry details. The
grid camera sidebar displays catalogued recording bytes, current filesystem space,
the configured quota and reserved free space on four lines below the camera list. Recording schedules, per-camera
recording switches and export are not provided.

On **Cameras**, **Retention (all cameras)** sets the history to keep in whole
minutes. The default is **60 minutes**, including configs without a
`retention_minutes` field. **Apply** saves the setting; **0 / Disabled** turns
off time-based removal. About once per second the controller finds inactive
files whose known end time is at or before `now - retention_minutes`. It never
deletes a segment overlapping that cutoff or one with unknown duration.
Playback, lookahead and extraction handles protect their files until released;
expired files are retried later. Byte quota and reserved-space pruning still
apply independently, even with retention disabled.

**Delete all closed recordings** asks for confirmation and covers every camera,
including removed cameras and interrupted/legacy history. It snapshots files
that are not being written when the controller receives the request. Playback
and seek extraction stop and release their handles before deletion begins.
Active files and files finalized after that snapshot remain; live reception,
recording and analytics continue. Progress reports checked, deleted, protected
and failed files. **Cancel deletion** stops subsequent batches; it cannot restore
already removed files. Each batch handles at most 32 entries and checks a 10ms time budget between
files. Individual filesystem/database operations can exceed that budget. External
files and symlinks are never followed or removed. Other SDK consumers holding
files can cause a partial result; release them and repeat the operation.

## Recording timeline and playback

Single view on **Live** contains the recording timeline. Double-click a tile
(or choose one view) to enter it; double-click or Esc returns to the grid.
Single view hides the grid toolbar, camera list, camera selector and storage panel.
Its vertical timeline is on the left, newest at the top, with **Back to Live** above. The timeline sidebar is 165–225px wide.
There is no coverage legend or duplicate live status outside the image. The camera name uses the large GPU text overlay in single view; black title strips
remain only in the grid. The timeline spans the oldest finalized recording to
now, with green recorded intervals and grey gaps. Coverage refreshes each second.

Click the timeline to start playback at that wall time, including when previously
paused. Gaps resolve forward with a notice. Click the video to pause/resume;
there are no Play/Pause/Stop buttons. Clicking live video freezes only display;
recording and inference continue. Another click resumes the latest live frame.
**Back to Live** releases recording playback and shows the current live stream.
Playback at the archive tail waits for finalized segments instead of switching
to live automatically. A camera change returns to that camera's live view.

Recording frame datetime (`yyyy-MM-dd HH:mm:ss`) is shown at the lower right;
live uses bright green `Live`, or yellow `Live · Paused`, at the upper right.
Paused video shows a translucent grey play triangle at its center, 20% of the
displayed video height. The timestamp belongs
to the displayed decoded frame, not a timer estimate. The old time visibility
preference is ignored. Camera name and optional stream information use GPU text
geometry, without video readback or glyph texture uploads.

Hovering temporarily replaces the main image with a keyframe preview and its
actual frame time, labelled `Preview`. Recording playback pauses during hover
and resumes its prior state on leaving; paused playback stays paused. Live
returns to the newest frame, or its held frame if paused. Clicking the timeline
commits a new playing position. There is no popup window. Loading, gaps and
errors retain the preceding frame rather than clearing to black. The displayed
preview timestamp continues to identify that retained frame until a new image arrives.

One preview worker coalesces requests for 75ms and decodes a key unit at or
before the requested position. VA/VPE produces a GPU image bounded to 1280×720;
DMA-BUF imports feed the main EGL renderer. An eight-image LRU bounds memory.
Only the latest request may publish; camera changes, leaving hover and deletion
cancel work and release the cache. No full video pixels are copied to the CPU.
A preview may precede the requested time by a GOP.

Drag a region on live, held-live or recorded video to search. The displayed
surface is pinned until crop extraction finishes. Dragging pauses recorded
playback; Esc cancels and restores its previous state. Click/double-click/drag
are distinguished using the desktop thresholds. Search uses the existing bounded
crop and embedding pipeline. Returning from Search restores the camera and
playback mode.

Playback crosses file boundaries and recording-session changes automatically.
The next recording is opened and prerolled using a second decoder, with at most
two playback decoders and one held preroll frame in the lookahead worker. Both
workers use one stable preview slot. The next worker remains paused and cannot
write to that slot before handover. Old requests are invalidated by output
slot generations. Files wholly covered by already-played time are skipped;
partially overlapping files are sought forward to avoid replaying old time.

Gaps are skipped immediately rather than played as elapsed blank time. At the
latest finalized recording, the last frame remains visible with a waiting
message. A registered camera, including one awaiting reconnection, resumes as
new fragments finalize. Removed cameras end playback at the archive tail.
Active/interrupted files are not opened; playback therefore trails reception
by approximately one segment plus finalization delay when following the tail.
Unavailable, corrupt or unsupported files are skipped with a notice; each is
attempted only once per open/seek request. A decoder preparation timeout is five
seconds. Fatal catalog errors appear in playback status without stopping live
reception, recording or analytics.

New recordings derive segment start times from a receiver wall-clock anchor
and the first encoded sample supplied by `splitmuxsink::format-location-full`.
The timestamp is inserted atomically with the open recording; finalized fragment
offsets confirm it at close. This avoids rejecting analysis near a boundary while
file-creation time is later than the media clock.
Existing recordings retain their previous registered timestamps. These are
receiver-side dates, not camera capture times or synchronized surveillance
clocks. Container-only duration indexing runs off the GUI thread, at most one
unknown-duration file per refresh, with a two-second preroll wait and no video
decoder. Failed probes are not retried during the catalog's lifetime. Unknown
lengths remain omitted from coverage instead of inventing a ten-second length.
The playback datetime is based on the preview frame's stream timestamp;
playback pacing uses GStreamer's monotonic media clock.

Playback remains normal-speed H.264/H.265 under the existing progressive NV12,
2560×1440, BT.709 limited-range restrictions. Audio is not added.
Leaving Live pauses recording playback and returning restores its prior state. Live decode/AI/recording continue,
requiring up to 16 live decoders plus two playback decoders and one seek-preview
decoder. The VA decode,
VPE preview and DMA-BUF/EGL display path uses no CPU pixel mapping/upload and
has no software decoder fallback. Currently opened, lookahead and extraction files are pinned against both
quota and time pruning; stop, seek and camera change release unneeded handles
asynchronously. Cached thumbnails own only small GPU surfaces, not file pins.

## Recorded detection overlays

Each analyzed frame saves normalized boxes, confidence, person/track identifiers,
camera, recording session, analysis run and media-aligned wall time in SQLite.
Empty detection frames are saved too. This is independent of the slower embedding
collection and requires no inference during playback. Existing recordings without
these records play normally without boxes; there is no historical backfill.

The main recording view, seek previews and search-result clips share the renderer.
They use the latest preceding analysis in the same recording/session, holding
boxes for at most twice the analysis interval (capped at two seconds). Empty
frames clear boxes; seeks and deletion invalidate cached results. There is no
coordinate interpolation or cross-camera identity matching. Labels and colors
match live person IDs. Only `#ID` is shown, with no confidence percentage;
unassigned IDs and all grid labels are hidden. Colored borders use 40% opacity
and their black outlines 20% opacity. ID text uses 60% opacity and its backing 30%.

`Catalog::enqueue_analysis` sends records to a bounded 1,024-frame writer queue.
Transactions save up to 64 frames at a time, flushing every 250ms. Overflow drops
the oldest unsaved frame. AI Statistics reports `Analysis DB save`,
`Analysis save queue` and failures; storage errors are also shown in view status.
The additive `analysis_frames` / `analysis_objects` tables contribute to the
storage quota estimate and are pruned with their recordings. Late completions cannot recreate data for deleted recordings.
`Catalog::analysis` retrieves bounded time ranges off the rendering thread.
`Catalog::opened` accepts an optional media start timestamp while retaining
source compatibility through its default argument. Public `Plane` and
`GpuPreviewImage` metadata now includes recording identity
and interval bounds; SDK consumers must rebuild.

## Framework interfaces and migration

`amdva/streaming/catalog.hpp` adds `Recording`, `RecordingQuery`, `StorageStatus`
and `RecordingFile`, plus paged `Catalog::list`, historical cameras, safe
`open_recording`, duration updates and interrupted-session accounting. An opened
file owns a read-only FD and carries its identity; it can outlive the catalog,
while pruning protection applies within the owning catalog. The controller
exposes these operations and source updates without exposing its SQLite handle.

The catalog transactionally migrates the original four-column schema. Existing
inactive rows become `legacy`; previously active rows become `interrupted`.
New `finalized` state requires a fragment-close message, not a destructor or a
timeout. Legacy playback must successfully parse/decode; it is never relabelled
as proof of a complete original recording. Known media duration is supplemented
from successful playback. `opened` and `closed` remain source-compatible with
existing SDK callers. Rebuild SDK clients; older binaries using four-column
inserts cannot write a migrated catalog.

`amdva/streaming/playback.hpp` retains the Qt-free, file-based `PlaybackSession`
and adds `TimelinePlaybackSession`. Construct the latter with a VA display and
`Catalog &`; the catalog must outlive the player and the display is retained.
Call `open(camera_id, unix_ms, play, follow)`, `seek(unix_ms)`, `play`, `pause`,
`stop` and `follow(expected)` to control it. Defaults are `play=true` and
`follow=true`; standalone archives should use `follow=false`. Status includes
camera, requested/actual wall time, state, notice/error and frame/transition/
skip counts. Counts restart on open/seek. Operations are asynchronous; status
is a thread-safe snapshot. `slot()` always returns the same `PreviewSlot` (`amdva/streaming/preview.hpp`).

Catalog additions are `coverage(camera_id)`, `resolve(camera_id, unix_ms)`,
`next(recording)`, `media_start(name, unix_ms)` and `index_unknown(camera_id)`.
Coverage unions touching/overlapping half-open intervals. Resolution prefers
the latest-starting containing file, otherwise the next available file, with
filename ordering as a stable tie breaker. Lookups are independent of listing
page limits. `Controller::recording_catalog()` exposes the controller-owned
catalog for shared playback services; its reference must not escape that
controller's lifetime. Keep blocking catalog operations off the GUI thread.

The additive nullable `media_started_ms` column is migrated transactionally and
uses `created` for historical rows without media timing. Rebuild SDK consumers:
public catalog layout has changed. Existing file-based source APIs remain
available, including the original two-argument playback `open` overload.
Consumers must release EGL/GL references before destroying a playback session.

Both playback session types add `stop_and_release()`, returning a future ready
after decoder teardown and input-handle release. `KeyframePreviewSession` is
also Qt-free: construct it with the retained display and a catalog that outlives
it, call `request(camera, unix_ms)`, and poll `status()` for the request ID,
state and immutable `GpuPreviewImage`. An exported surface descriptor transfers
FD ownership to the caller. `cancel()` returns a release-barrier future and
clears pending work and GPU cache. Shared image ownership survives cache
eviction; renderers must finish GPU sampling before dropping that ownership.

`Catalog::begin_delete(optional_cutoff_ms)` creates a snapshot job, and
`delete_batch(job)` advances it with handle, active-file and path checks.
`deletion_generation()` lets image consumers invalidate stale results.
`Controller::retention_minutes`, `delete_closed_recordings` and
`cancel_recording_deletion` enqueue management operations; `deletion_status`
returns progress. An optional shared release future gates manual removal after
its snapshot: GUI callers use it to coordinate asynchronous player/preview
teardown without blocking the event loop. Catalog and controller operations
are reusable by headless applications; confirmation and rendering belong to
the desktop app.

The [file-based SDK example](../../sdk/playback.cpp) and
[timeline SDK example](../../sdk/timeline.cpp) and
[GPU keyframe-preview example](../../sdk/keyframe-preview.cpp) demonstrate Qt-free
consumers. Each standalone example owns its catalog exclusively; never point
it at an active NVR's recording root.

## Validation

The subsequent vertical-timeline/display refinement passed all 33 tests and a
90-second 16-channel 1440p GUI check. Recorded analysis remained 1.998–2.015 Hz
per camera, with no startup tracking drops or copy-audit violations. Checks
include hidden single-view controls, four-line grid storage status, Esc return,
paused-live rendering and retention of the preceding image while a preview loads.
The management check passed 100 hover requests (p95 193 ms, maximum 286 ms),
expiry/deletion and stream-information toggling. Search checks passed text search,
inline playback, live/held-live/recorded region selection and deletion. Local logs
and screenshots are under `artifacts/view-layout/`.


The [unified-view validation record](../benchmarks/unified-view-2026-10-22.json)
covers the 2026-10-22 implementation. Original 1440p HEVC videos ran on 16
channels for 600 seconds while switching grid/live/playback/hover modes.
Tracking stayed at 1.983–2.017 Hz per camera in stable windows; persisted
analysis measured 2.000 Hz per camera across recording boundaries. Both queue
drop counters remained zero, including startup. Peak process RSS was 7,425 MiB.
The GPU copy audit passed. This is a ten-minute check, not a long soak.

The 33-test suite passed, followed by the affected catalog/timeline/Unicode
contracts. The GUI checks covered all three region-search modes, text search,
inline playback, hover restoration, actual mouse clicks, archived-camera
selection, expiry and deletion. With Japanese camera names, 100 in-place hover
samples measured p95 160 ms / maximum 289 ms with zero copy/GL-image violations.
The cold preview test separately passed 100 H.264 1080p / HEVC 1440p requests.

Prepared local demo: `bash artifacts/unified-view/run-demo.sh`. It uses the
original videos, port 18570 and isolated settings/recordings under
`artifacts/unified-view/`; these artifacts are not committed.

The numeric results below describe earlier UI revisions. The unified view replaces
their popup, clock-toggle and paused-seek behavior as described above.

`playback-error-contract` exercises malformed recordings, repeated stop/open,
error isolation and playback-handle release. Catalog tests cover migration,
legacy/interrupted/missing states, page cursors, date filters, pins and quota
pruning, in addition to the original root/ownership/symlink checks.

Run the real GUI scenarios with encoded RTSP fixtures:

```bash
source scripts/env.sh
python3 examples/nvr/tools/vms_soak.py artifacts/nvr-local.json \
  artifacts/nvr-model/bus1080.h264 artifacts/nvr-model/blank1080.h265 \
  --seconds=600 --restart-at=180 --output=artifacts/vms-ten-minute
```

The current GUI validator checks timeline controls, live reception/decode/AI,
continuous playback, paused wall-time seeking, reconnection and codec changes.
Allow at least 130 seconds with the wrapper. It retains resource growth guards
after a 240-second warmup, validates finalized MKVs, and checks the GPU copy
auditor. Shorter runs do not establish long-term resource stability. Qt widget
captures can omit native video windows; separate framebuffer captures verify
the imported playback image.

`timeline-catalog-contract` checks wall-time resolution, unioned coverage,
missing/active/unknown recordings and a 10,005-file history. The offscreen
`timeline-widget-contract` checks 64-bit dates, edge mapping, hover/drag behavior,
empty state and painting. `amdva-timeline-validate H264.mkv H265.mkv NEW_ROOT`
uses two-second 30fps fixtures to check six boundary transitions, accurate
paused seeking, gaps, corruption, following appended recordings, camera changes,
rapid requests, and release of playback/lookahead pruning pins. It measures
frame intervals with a 250ms guard and can run under the copy-audit preload.

The [timeline validation record](vms-timeline-validation-results.json) identifies
source/runtime hashes and local artifacts. The standard build passed all 25
CTest cases. A 130-second GUI run with two live cameras, AI and ten-second
recordings survived an RTSP restart with swapped codecs, crossed ten playback
boundaries and finalized 26 valid recordings. The maximum interval between
swaps presenting new playback frames was 64.3ms, below the 250ms test guard;
the GPU copy auditor reported zero violations. The standalone hardware test
measured at most 37.5ms between preview submissions across its six boundaries,
then verified following a newly finalized fragment. Installed SDK examples
played three migrated archive files continuously and retained file-based
playback compatibility, without a Qt dependency in the streaming library.

A subsequent 65-second two-camera GUI regression verified automatic paused
preview on initial selection and preservation of the selected wall time when
switching cameras while paused, followed by resumed playback. Both that run's
copy audit and the offscreen timeline widget contract passed.

These checks do not establish capacity for 16 live plus two playback decoders,
or long-term memory/FD stability; the GUI run is shorter than the resource
guard's 240-second warmup. The results below predate the timeline redesign and
document the earlier file-list UI.

### Recorded results

The [validation record](vms-validation-results.json) identifies the code revisions,
hardware, runtime hashes and local artifact paths. All 17 CTest cases passed.
A 600-second run received 20 cameras, selected 16 live streams and exercised
one playback decoder, with an RTSP server restart and codec changes. All 1,201
recordings finalized and passed media inspection. After the 240-second warmup,
maximum growth was 9,652 KiB RSS, 18,360 KiB GPU allocation and four FDs, within
the existing guards. The frozen runtime predates the final small refinements
listed in the validation record.

The final implementation passed a separate 150-second, 25-phase GUI run with
317 valid finalized recordings, including camera add/remove, retained history,
16-view replacement, swapping, clearing and persisted configuration checks.
Both GUI runs completed without GPU copy auditor violations. Installed SDK
playback decoded 300 frames for each codec. A build with `AMDVA_BUILD_NVR=OFF`
also completed without a Qt dependency. These are correctness and resource
checks, not a performance certification.

### Recording management and GPU seek previews

`recording-management-contract` checks expiration by segment end, cutoff
overlap, unknown lengths, pins, snapshot protection for active/new files,
bounded batches, missing files and symlink/external-file protection. Config
tests cover the 60-minute default, persistence of zero and invalid values.

```bash
source scripts/env.sh
python3 examples/nvr/tools/vms_management_validate.py artifacts/vms-timeline-input.json \
  artifacts/nvr-model/bus1080.h264 artifacts/nvr-model/blank1080.h265 \
  artifacts/vms-timeline-fixtures/h264.mkv \
  --output artifacts/vms-management-check
```

This creates a separate recording root and RTSP server. The hardware GUI check
tests retention persistence, expiry protection/release, image-click playback,
100 hover-to-frame-swap measurements, delete-all release barriers and
preservation of active recordings while two live decode/AI pipelines continue.
`amdva-preview-validate H264.mkv H265.mkv NEW_ROOT` separately tests 100 cold
thumbnail requests, bounded caching, cancellation, gaps, corruption and deletion
invalidation. Its fixtures are two-second 30fps files with one-second GOPs.
Both checks can enable the copy auditor; the GUI check also audits GL pixel
readback and CPU texture uploads. Results and limitations are recorded in
[vms-management-validation-results.json](vms-management-validation-results.json).
These short runs do not establish 16-live-plus-three-extra decoder capacity
or long-term resource stability.

The standard build passed all 26 CTest cases (23 in the isolated build). The
standard GUI run completed 100 hover-to-swap samples with a 191.9ms 95th
percentile and 288.1ms maximum, with zero GPU-copy/GL-image audit violations.
It also verified retention, cancellation, image-click controls and deletion of
five closed files while retaining active fragments and continuing live AI.
The standalone two-second-GOP/B-frame test completed 100 cold H.264/H.265
requests with an 88.8ms 95th percentile, then checked corruption and deletion
invalidation. Installed SDK consumption produced a 320×180 GPU image without
a Qt dependency in the streaming library. These are synthetic input-to-swap
measurements, not physical display latency measurements.

A subsequent preview-retention GUI regression verified 99 loading requests
kept the previous GPU image and 91 mouse moves changed the popup position.
Wayland protocol logs confirmed native subsurface position updates. Deletion
invalidation still cleared retained pixels. The full management GUI scenario
passed again with zero copy/GL-image audit violations; see the follow-up section
in the management validation record.

## Person search

See [NVR semantic search](nvr-search.md) for native EmbeddingGemma 2 setup,
collection policy, text and region queries, and inline result playback.
