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

## Recording timeline and playback

The **Recordings** page selects one camera at a time, including removed cameras
with retained history. There is no file list, date filter or per-file slider.
The timeline always spans that camera's oldest available finalized recording to
the current local time. It refreshes coverage approximately once per second and
has no zoom controls or 10,000-fragment truncation. Long histories necessarily
have coarser mouse precision at a fixed window width.

Green shows known playable coverage; grey shows gaps or unavailable coverage.
A white marker follows the playback position. Hovering shows a vertical guide
and `yyyy-MM-dd HH:mm:ss` label inside the timeline. Click, or release a drag,
to seek by absolute time; dragging does not repeatedly restart the decoder.
A gap selection jumps forward to the next available recording and displays a
notice. Playback stays paused when seeking while paused; other seeks start
playback. Camera changes preserve the selected wall time and immediately
load its image for the new camera. Playing/waiting playback continues; otherwise
the image is shown paused. The first camera selection uses its earliest
available time. If that time has no recording, the usual forward gap resolution
applies. Stop clears the image and preserves the selected time.

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
and `splitmuxsink` media offsets, rather than asynchronous file-creation timing.
Existing recordings retain their previous registered timestamps. These are
receiver-side dates, not camera capture times or synchronized surveillance
clocks. Container-only duration indexing runs off the GUI thread, at most one
unknown-duration file per refresh, with a two-second preroll wait and no video
decoder. Failed probes are not retried during the catalog's lifetime. Unknown
lengths remain omitted from coverage instead of inventing a ten-second length.
The playback datetime is based on the preview frame's stream timestamp;
playback pacing uses GStreamer's monotonic media clock.

Playback remains normal-speed H.264/H.265 under the existing progressive NV12,
1080p, BT.709 limited-range restrictions. Audio and playback AI are not added.
Leaving the recording page pauses playback. Live decode/AI/recording continue,
requiring up to 16 live decoders plus two playback decoders. The VA decode,
VPE preview and DMA-BUF/EGL display path uses no CPU pixel mapping/upload and
has no software decoder fallback. Only the currently opened and lookahead
files are pinned against quota pruning; stop, seek and camera change release
unneeded handles asynchronously.

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
is a thread-safe snapshot. `slot()` always returns the same output slot.

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

The [file-based SDK example](../examples/sdk/playback.cpp) and
[timeline SDK example](../examples/sdk/timeline.cpp) demonstrate Qt-free
consumers. Each standalone example owns its catalog exclusively; never point
it at an active NVR's recording root.

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
