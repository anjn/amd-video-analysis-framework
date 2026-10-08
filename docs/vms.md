# Desktop video management

The NVR desktop now has **Live**, **Recordings** and **Cameras** pages. It uses
Qt Widgets and the existing hardware EGL renderer. RTSP reception, recording,
reconnection, selected-camera analytics and optional background analytics remain
services in `amdva_streaming`.

## Live monitoring

Choose 1, 4, 9 or 16 views. Click a tile, then choose its camera in the toolbar,
or double-click a camera in the left list. **Clear tile** removes that mapping.
Assigning a camera already used elsewhere exchanges the two tiles. Empty and
disconnected tiles keep their position. Camera titles, connection states and the
selected border remain visible over the grid.

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

**Cameras** provides add, edit and remove actions. Name changes preserve the
connection and source ID. URL changes retire only that camera's current receive,
decode and recording session, then reconnect using the new URL. Removing a
camera stops its reception and clears its mappings, without deleting history.

The table retains connection, recording, analytics and telemetry details. The
bottom bar displays catalogued recording bytes, current filesystem space, the
configured quota and reserved free space. This release displays storage policy;
it does not add recording schedules, per-camera recording switches, manual
recording deletion or export.

## Recording search and playback

Search by camera and local date/time. Defaults are all cameras and the current
local day. Results are newest first, with 50 rows per page and stable keyset
cursors. Columns show start time, historical camera name, duration, size, state
and filename. Removed cameras remain in the history filter. The time filter
includes known-duration recordings overlapping its start boundary.

Select a single camera to show recording coverage, then click a covered time to
open and seek that recording. The timeline loads at most the latest 10,000
segments in the search range and explicitly indicates truncation; narrow the
range to inspect older detail. Unknown durations are not drawn as invented
coverage. A row can still be opened before its duration is known.

Double-click an available finalized or legacy row to play it. Controls provide
play, pause, stop, a per-file seek slider, and previous/next recording. Accurate
flush seeking decodes from a suitable preceding keyframe and presents the
requested position. Paused seeks render a new preroll image without resuming.

Adjacent fragment numbers in the same session automatically continue. Missing
fragments, a new recording session or unavailable files stop continuation and
show a notice; **Next recording** explicitly moves on. Reaching the end of the
loaded/finalized range stops playback. Active and interrupted recordings are
not opened. The local start time describes receiver-side file registration,
not camera capture time or a synchronized surveillance clock.

Playback supports one recording stream, at normal speed, using H.264/H.265
within the NVR's progressive NV12, 1080p, BT.709 limited-range restrictions.
Audio and playback AI are not added. Live decode/AI/recording continue, requiring
up to 16 live decoders plus one playback decoder. Leaving the recording page
pauses playback. Hidden live previews pause independently of live decode/AI;
preview/draw/swap telemetry is marked inactive instead of reporting a display
stall during page changes or single-view focus.

The playback worker uses `fdsrc`, Matroska demux, the actual VA codec element,
and the same VPE preview preparation as live video. The Qt renderer imports the
preview pool through DMA-BUF/EGL. There is no CPU pixel mapping/upload or
software decoder fallback. Failed playback reports an error without changing
live controller state. Opening and querying recordings happens outside the GUI
thread. The currently opened file is protected from the catalog's quota pruning
until the playback session releases its handle.

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

`amdva/streaming/playback.hpp` exposes Qt-free `PlaybackSession` operations
`open`, `play`, `pause`, `seek`, `stop` and a status snapshot. Its preview slot
uses the existing three-plane state/fence ownership protocol. Consumers must
release their EGL/GL references before destroying the session. The shared
`prepare_preview` helper keeps VPE preparation consistent with live rendering.
`Controller::preview_sources` selects which already-selected sources need
preview images; it changes neither recording nor analytics admission.

The [SDK playback example](../examples/sdk/playback.cpp) demonstrates a Qt-free
consumer that releases completed preview planes. As a standalone catalog owner,
it requires a recording root that is not owned by another running recorder.

## Validation

`playback-error-contract` exercises malformed recordings, repeated stop/open,
error isolation and playback-handle release. Catalog tests cover migration,
legacy/interrupted/missing states, page cursors, date filters, pins and quota
pruning, in addition to the original root/ownership/symlink checks.

Run the real GUI scenarios with encoded RTSP fixtures:

```bash
source scripts/env.sh
python3 tools/vms_soak.py artifacts/nvr-local.json \
  artifacts/nvr-model/bus1080.h264 artifacts/nvr-model/blank1080.h265 \
  --seconds=600 --restart-at=180 --output=artifacts/vms-ten-minute
```

The validator exercises 20 cameras, 16 live selections and one recording
player, both codecs, continuation, paused seeking, focus, grid restoration,
camera rename and URL replacement, GUI camera addition/removal with preserved
history, replacement at the 16-view limit, duplicate-assignment swaps and
clear/reassign with persisted layout checks. Allow at least 130 seconds for
these scenarios. The server restarts with swapped codecs.
It keeps the existing 64 MiB RSS/GPU and FD-growth guards after a 240-second
warmup, validates finalized MKVs, and checks the GPU copy auditor. It does not
impose latency/FPS acceptance thresholds. Framebuffer captures are diagnostics
from the test tool, not production frame-processing operations. Qt widget
screenshots alone may omit native QWindow content; the separate live/playback
framebuffer captures verify the actual imported video images.
