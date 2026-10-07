# Decode and presentation telemetry

The framework measures local receive-to-decode and receive-to-presentation
latency, throughput, frame intervals, observed stops and known local discards.
These measurements are observations. They do not classify a stream as passing
or failing, reconnect a source, or change frame admission policy.

## Timing boundaries

| Stage | Observation |
| --- | --- |
| `input` | First RTP packet of this frame observed at rtpbin's receive sink, before its jitter buffer |
| `access_unit` | Parser's compressed access unit reaches the configured pad |
| `decode_input` | Compressed buffer reaches the decoder sink pad |
| `decode_output` | Raw buffer reaches the decoder source pad |
| `preview_ready` | NVR's VPE preview processing completes |
| `draw` | NVR submits the image's GL drawing commands |
| `swap` | QOpenGLWindow emits `frameSwapped` for the paint containing this image |

The input clock is the receiver process's monotonic clock. This is a userspace
RTP observation, not a NIC hardware timestamp, camera capture timestamp or wall
clock. TCP framing and earlier transport processing are outside this boundary.
RTP receipt before jitter buffering includes jitter-buffer residence in the
receive-to-decode measurement. Parser reception and decoder input are recorded
separately, so queueing before the decoder can be distinguished from its input-
to-output path, which includes reordering and internal queues.

Decoder output means buffer delivery, not an independently synchronized measure
of physical GPU decode completion. `swap` means the Qt buffer-swap notification,
not monitor scanout or photon emission. No `glFinish`, VA surface sync, HIP sync,
or CPU pixel mapping is added for timing. Existing processing synchronization is
unchanged. A frame's first drawing and first observed swap are counted once;
painting the same image again does not increase the new-image FPS.

## SDK and GStreamer use

`amdva/telemetry.hpp` exposes a Qt/GStreamer-independent `Telemetry` collector,
`FrameTrace`, `TraceToken`, stage availability and `TelemetrySnapshot`.
`Telemetry::monotonic_ns()` is the production clock. Applications manually mark
stages using this same clock, or inject a clock in deterministic tests. Never
subtract a PTS, RTP timestamp, NTP timestamp or foreign process clock from these
monotonic observations.

`amdva/gst_telemetry.hpp` adds `PipelineTelemetry` and a value-copying
`FrameTraceMeta`. Adapters own their pad probes and signal handlers. Attach them
before PLAYING; stop their pipelines before destroying the adapters. An adapter
retains the observed GStreamer objects until detachment. Frame metadata can
outlive both pipeline and adapter, retaining its collector without retaining
image buffers.

```cpp
auto collector = std::make_shared<amdva::Telemetry>("camera-1");
collector->begin_generation(1);
amdva::PipelineTelemetry timing(collector);
timing.attach_rtsp(rtsp_source);
timing.attach_access_units(parser_src_pad);
timing.attach_decoder(va_decoder);
// Set PLAYING, periodically read collector->snapshot().json(), then set NULL.
```

For RTP pipelines without `rtspsrc`, `attach_rtp(before_jitter, after_jitter)`
provides equivalent correlation. `attach_stage(pad, stage)` marks an existing
trace at other processing boundaries. A renderer retrieves a `TraceToken` with
`frame_trace(buffer)` and calls `mark(Draw)` and then `mark(Swap)` at its actual
backend notifications. A sink probe alone cannot measure presentation.

For separate appsrc decode pipelines, copy buffer headers/metadata, rebase
PTS/DTS as needed and call the decode adapter's `submit(buffer)` before pushing.
The submitted PTS is associated with the original trace. The decoder sink probe
marks actual decoder entry. The source probe first uses the frame identity
preserved by the decoder, then falls back to unambiguous output PTS. It never
uses FIFO order or a nearest-PTS guess. This also handles decoders adjusting an
output PTS while retaining the exact frame metadata. Native NVR already integrates these operations. See the external
[SDK telemetry example](../examples/sdk/telemetry.cpp) for a standalone H.264 or
H.265 RTSP pipeline; it requires the compatible VA/GStreamer runtime but no Qt.

The RTSP adapter expects one video track per RTSP source. For multitrack
RTP pipelines, bind separate collectors/adapters to the selected track pads.
Matching uses SSRC/RTP timestamps to relate first arrival to post-jitter PTS and
then parser AU PTS. Reordering and timestamp/sequence wrap are supported within
the retention window. Missing or ambiguous PTS, unsupported timestamp rewriting
and unmatched outputs cannot produce an invented receive latency. They increase
counters; unavailable distributions have zero samples and null durations.
The local decoder-output count is still measured for unmatched outputs.

Correspondence tables are bounded to 4,096 entries and approximately 30 seconds;
expiry and capacity eviction are visible. Decoder submissions retired without
an observed matching output have their own counter: they are not proof of a
hardware decoder drop. Flush and replacement stream-start events retire timing
generations; explicit reconnects call `begin_generation`. Generation numbers
are monotonically advanced within a collector, even when a caller supplies an
older requested number. Old-generation stage notifications are rejected. Timing generations are
collector-local and may advance on decoder flush independently of the
application source generation; use timing tokens for correspondence.

## Statistics and NVR integration

Each snapshot contains `continuous` and `background` groups. Input and AU
counts cover continuous reception for all sources, including hidden sources;
sparse background decode outputs are kept in the background group. Audio RTP
is excluded. A partially received frame still counts as input even if it never
produces an AU. The groups provide per-stage
counts, FPS, age, stop statistics and inter-frame interval distributions.
Latency distributions cover input→decode, decoder input→output, input→draw,
input→swap, decode→preview, preview→draw and draw→swap. Durations are milliseconds.
Counts last for the collector's lifetime; reconnects reset recent windows and
frame correlations. Removing and re-adding a source creates a new collector.

Recent distributions retain up to 4,096 samples in ten seconds and use the
nearest-rank P95/P99. `samples`, `effective_window_ms` and `truncated` describe
coverage. FPS is a rolling ten-second arrival count divided by the elapsed
active observation window. No sample is represented by a false zero latency.
Copies recover known ancestor timestamps from the collector when a later stage
is marked, so a renderer may pass token copies between its draw and swap hooks.
Snapshots sort only bounded metadata samples; the media hot path performs no
per-frame filesystem I/O. Call snapshots at a monitoring rate such as once per
second, rather than once per packet/frame.

After an active stage has produced a first frame, an update gap of at least
`stall_ms` is recorded as one stop. The full gap duration is counted, including
the time before the threshold. Ongoing stops update through snapshots. Pausing
or retiring a stage ends its current observation interval. Stages without any
first observation expose an unavailable age rather than guessing a stop time.
Sparse background keyframe inference has separate rates/intervals and does not
trigger continuous-stage stop detection.

Native NVR exposes `Controller::telemetry()`, shows actual decode/swap/AI FPS
and input→decode/input→swap P99 columns, and provides full statistics in the FPS
cell tooltip. Display observation pauses while hidden, unexposed or minimized.
Headless mode leaves preview/draw/swap unavailable. Existing `Status::decoded`
and `display_fps` retain their legacy preview-processing meaning for API
compatibility; use the new `decode_output` and `swap` stage counters instead.

```json
"telemetry": { "enabled": true, "stall_ms": 1000 }
```

This NVR configuration is optional and defaults as shown. Disabling telemetry
removes measurement probes; snapshots explicitly report `enabled=false`.
Counters distinguish compressed queue discards, overwritten unprocessed latest
frames, undisplayed previews, unobserved presentation submissions, unmatched
RTP/decode correspondence, ambiguous/missing PTS, expiry, capacity eviction,
stale generations and invalid monotonic stage ordering. These counters have no
operational pass/fail interpretation.

`amdva-streams` and the GUI validator add a separate top-level `telemetry` array
to their periodic JSON. AI result publication is unaffected. `nvr_soak.py`
preserves the detailed JSON-lines log and includes final telemetry in its
summary without adding latency/FPS acceptance checks. Generate a compact report:

```bash
python3 tools/timing_report.py artifacts/timing-nvr-hour-final \
  --start=720 --output=artifacts/timing-hour-report.json
```

The report summarizes rolling-window P99s, not a whole-run P99. It also records
FPS, interval P99s and process CPU/RSS/GPU usage. CPU percent is relative to one
logical core. Compare telemetry-on/off runs on the same fixtures to observe
measurement overhead without imposing a threshold.

## Validation tools

`telemetry-contract` uses an injected clock for intervals, percentiles, rolling
bounds, stop accounting, duplicate notifications, generation retirement and
disabling. `gst-telemetry-contract` exercises controlled stage delays, RTP wrap,
partial video frames, audio exclusion, metadata ownership, adjusted output PTS,
reordered output, missing metadata, ambiguous PTS, flush, table bounds and
copy-on-write buffer lists.
The framework pipeline test also checks timing metadata through AMD task
elements, watermark, metadata aggregation and heterogeneous mux/demux.

`amdva-validate-timing-rtp INPUT.mkv h264|h265 normal|reorder|loss` repacketizes a
local Matroska B-frame fixture, injects packet reordering/duplication or loss,
and decodes through the actual VA decoder. It checks correspondence correctness
without an operational latency threshold. Fixture timestamps come from the
container; the encoded-only NVR RTSP fixture server deliberately does not
support B-frame timing and must not be used for that test. RTP timestamp and
sequence offsets exercise wrap in this tool.

`nvr-timing-tests CONFIG` renders actual RTSP sources, hides the window,
re-exposes it, minimizes it and remaps it. It checks that decode continues while
presentation is paused and that repeated drawing never inflates the new-image
count. The explicit hide/remap restoration accommodates Wayland compositors
that do not implement a programmatic unminimize request.

## Recorded hardware validation

The machine-readable [validation report](telemetry-validation-results.json)
records fixtures, runtime hashes, stage counts, correspondence counters and
observed latency/FPS/resource statistics on Ryzen AI 9 HX 370 / Radeon 890M.

All 16 CTest contracts passed. The framework GPU copy audit retained timing
metadata through processing, watermark, aggregation and mux/demux without a
copy-audit violation. H.264 and H.265 B-frame fixtures each produced 60 decoded
frames with normal delivery and with injected reordering/duplication; injected
packet loss produced 59. All six cases retained exact input/decode
correspondence. A separate live renderer test covered hiding, showing,
minimizing, remapping and resizing. Installed standalone SDK examples processed
both RTSP codecs, and a Qt-free headless build reported presentation unavailable.

The NVR regression used 20 received/recorded sources, 16 selected sources and
four background slots, dynamic selection/add/remove, RTSP restart and codec
replacement. The 90-second regression finalized 45 valid recordings and
recovered receive/record activity 7.50 seconds after restart. The one-hour run completed 100 dynamic cycles, finalized 1,315 valid
recordings and recovered 7.00 seconds after restart. Maximum post-warmup growth
was 30,284 KiB RSS, 27,456 KiB GPU allocation and four FDs. It uses
a frozen intermediate runtime; its hashes and the later changes covered by
separate final-build tests are recorded explicitly. The intermediate runtime
lacked input correspondence after background keyframe header prepending; the
final comparison verifies the repaired path for all four hidden sources. Concurrent regression tests
ran during part of that long run. The sequential telemetry-on/off comparison
uses the final runtime after those jobs have finished.

The report's latency statistics summarize ten-second window P99 observations;
they are not whole-run percentiles. They and the single telemetry-on/off pair
are observations, without latency/FPS acceptance thresholds or a performance
guarantee. Existing soak recording and resource guards remain in place.

In the isolated final-runtime comparison, selected preview FPS medians were
29.88 with telemetry and 29.93 without it. Process CPU averaged 130.12% versus
123.94% of one logical core, a 6.18 percentage-point difference (4.99% relative).
These runs include the validator's snapshot/JSON output. The telemetry-on
input→decode and input→swap window-P99 medians were 31.75 ms and 66.21 ms across
selected sources and observation windows, respectively. Both 90-second runs
passed the existing recording/resource checks. All four hidden sources produced
background input→decode samples with zero unmatched decoder observations.
