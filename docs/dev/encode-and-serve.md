# Encoding, recording and serving

The framework keeps decoded frames in GPU memory. This page covers getting an
annotated stream back out as compressed video without touching pixels on the
CPU.

## The zero-copy path

```text
vah264dec -> VAMemory NV12 -> amddetect -> amdwatermark -> BGRA DMA-BUF
          -> amdencode ( import -> VA video processor -> NV12 VAMemory -> vah26xenc / vaav1enc )
          -> compressed access units (CPU: parsing, muxing, RTP only)
```

- `amdwatermark` draws on an NV12 VAMemory frame and outputs a linear BGRA
  surface exported as DMA-BUF. GStreamer's `vapostproc` cannot negotiate this
  legacy `memory:DMABuf, format=BGRA` form, so `amdencode` imports the DMA-BUF
  as a VA surface itself (no copy) and converts it to NV12 limited-range BT.709
  on the VA video processor.
- VAMemory input (NV12, BGRA, BGRx, RGBA) goes through `vapostproc` and is
  passed through untouched when already NV12.
- Only the compressed bitstream reaches the CPU. The copy auditor
  (`amdva/copy_audit.hpp`, `libamdva_copy_audit.so`) is the tool for verifying
  this on your own pipelines.

### CPU-produced frames

A frame produced in system memory (for example an OpenCV `Mat` pushed through
`appsrc`) has to be uploaded to a VA surface before the encoder can use it.
That upload is a CPU-to-GPU copy by nature; it is not zero-copy and the framework
does not hide it. Upload once (`vapostproc` accepts system-memory input and
outputs VAMemory) and keep all later stages on the GPU. If you produce frames
with the GPU, use the VA/EGL path below instead.

### GPU-rendered frames

To render your own graphics (a dashboard, overlays beyond `amdwatermark`) draw
into a BGRA VA surface and encode it:

1. Allocate with `VaSession::create_bgra`, export with `export_surface(surface, true)`.
2. Import into EGL with `amdva::EglDmaBufImporter` (`amdva/egl_import.hpp`), attach
   the texture to an FBO, draw, and wait on a GL fence.
3. Convert to NV12 on the VA video processor into a `gst_va_pool` buffer and push it
   to an `appsrc` feeding the encoder. `examples/resource-dashboard/renderer.cpp` is the complete
   example.

Mesa 25.2.x note: converting an RGB surface to NV12 over identical regions
silently switches to encoder-side conversion after 16 frames and then fails.
The dashboard allocates a 1920x1082 source, draws into rows 2-1081 and converts
that rectangle to 1920x1080, so the regions differ. `amdencode` converts a
surface it imports from the overlay, and it was validated over 600 frames of an
annotated stream without the failure; the exact trigger on that path is not
characterised, so if you write your own conversion on Mesa 25.2.x, keep the
source and output regions different.

## `amdencode`

```bash
... ! amdwatermark ! amdencode codec=h264 bitrate=6000 key-int-max=30 ! h264parse ! mp4mux ! filesink location=out.mp4
```

| Property | Default | Notes |
| --- | --- | --- |
| `codec` | `h264` | `h264`, `h265`, `av1` (needs `vah264enc`, `vah265enc`, `vaav1enc`) |
| `bitrate` | 4000 | kbit/s; the target, or the cap for `vbr` and `qvbr` |
| `rate-control` | `cbr` | `cbr`, `vbr` or `qvbr` (quality-defined VBR); `vbr`/`qvbr` spend fewer bits on easy scenes |
| `key-int-max` | 30 | frames between keyframes |
| `b-frames` | 0 | H.264/H.265 only; 0 keeps latency low |
| `signal-bt709` | true | Encode and signal BT.709 limited range (caps for all codecs; H.264 SPS VUI rewrite) |

Properties are writable in NULL/READY. Output is byte-stream, access-unit
aligned (AV1: OBU stream). There is no software fallback: when the hardware
encoder is missing the element fails the state change with a missing-plugin
error.

`signal-bt709` exists because the frame import drops the colorimetry, so the VA
encoders would label the stream BT.601. With it, the encoder input caps say
BT.709, which reaches the H.265/AV1 headers and the container colour fields, and
`amdva::signal_bt709` (`amdva/vui.hpp`) also rewrites the H.264 SPS VUI, leaving
slice data and timing untouched. The NVR player only accepts BT.709 (or unsignalled)
recordings. The import has no colorimetry, so the processor assumes BT.601 below
720 lines; encode at 720p or more when the source is BT.709.

The radeonsi AV1 encoder pads the width to a multiple of 64 and mis-sizes heights
that are not a multiple of 16 (1080 is decoded as 1082). Feed AV1 frames whose size
already meets both, as `Transcoder` does.

## Recording

Fixed-length files with `splitmuxsink` (see `examples/pipelines/annotate-segments.sh`):

```bash
... ! amdencode key-int-max=30 ! h264parse ! \
  splitmuxsink muxer=matroskamux location=out-%05d.mkv max-size-time=10000000000
```

Keep `key-int-max` below the segment length so every file starts with a keyframe.
Recording the camera's own compressed stream (no re-encode) is what the NVR does.
`amdva::streaming::SegmentRecorder` (`amdva/streaming/recorder.hpp`) is that
recorder as a reusable class: feed it H.264/H.265 access units from a keyframe with
`push()`, pump `poll()`, and it writes rolling Matroska segments while keeping the
`Catalog` (names, media start time, duration, interrupted segments) in step.
Retention and quota live in the catalog and controller (`docs/dev/nvr/vms.md`).
`SegmentRecorder` also writes AV1 (`RecordCodec::AV1`), and `push(sample, wall_ms)`
takes the receive time of the first sample when it went through a delaying stage.

### Re-encoding a recording

`amdva::streaming::Transcoder` (`amdva/streaming/transcoder.hpp`) re-encodes one
camera's H.264/H.265 access units on the GPU to save storage:

```
appsrc ! h26xparse ! vah26xdec [! tee ! appsink frames] ! queue leaky
       ! [vapostproc ! width,height] ! amdencode ! appsink
```

Options are the output codec (H.264, H.265, AV1), rate control, bitrate, an output
height (0 keeps the size; it never enlarges and keeps the aspect ratio) and the
keyframe interval. AV1 sizes snap to a width multiple of 64 and a height multiple of
16 (1080p becomes 1920x1088). `push()` the received samples from a keyframe on one
thread and drain `pull()` on the same thread into a `SegmentRecorder` opened with
the first output's caps. The four-frame queue before the encoder drops the oldest
frame when the video engine falls behind, so the output frame rate drops instead of
the decode stalling; `encoded()` counts the output. `max_fps()` caps the encoded
frame rate while running, to leave video engine time to other decoders. `false` from `push()` means the
input backlog limit was still reached, and the caller restarts at the next keyframe.
With `Options::frames`, `frames()` is an appsink of the decoded NV12 VAMemory frames
(latest only) so a display or analysis consumer shares the decode. The
source PTS travel through decode and encode as reference timestamp metadata, so the
output keeps the camera timeline; `latency_ms()` is the push-to-output delay. Pass
the receive time of the first output's PTS to `SegmentRecorder::push` so recorded
analysis stays aligned. `transcode-contract` re-encodes generated streams to all
three codecs and plays them back.

`amdva::streaming::RtspIngest` (`amdva/streaming/ingest.hpp`) is the matching
receive side: it pulls one RTSP camera as byte-stream, AU-aligned H.264/H.265 access
units into a bounded queue (`take()`), reports overflow and a five second video
timeout through `poll()`, and leaves reconnect policy to the caller. Ingest,
recorder and a VA decoder are the three pieces the controller composes per camera.

## Serving over RTSP

`amdva::RtspServer` (`amdva/rtsp_server.hpp`) wraps `gst-rtsp-server` for
appsrc-fed streams; `examples/sdk/transcode.cpp` is a complete decode, analyse,
encode and serve program:

```bash
transcode in.h264 model.mxr model-info.json --rtsp 8554 --mount /annotated --codec h264
# rtsp://127.0.0.1:8554/annotated
```

The launch description names the source `video` and the payloader `pay0`:

```text
( appsrc name=video is-live=true format=time do-timestamp=true ! h264parse config-interval=-1 !
  rtph264pay name=pay0 pt=96 config-interval=-1 )
```

`config-interval=-1` on the parser repeats SPS/PPS before every keyframe, so a
client can join mid-stream. H.264 and H.265 are served; AV1 over RTP is rarely
supported by clients, so use AV1 for files.

If your source is already encoded (files, camera restream), serve it without
decoding: `amdva-rtsp-files` does exactly that for H.264/H.265 files.
The dashboard (`docs/dev/resource-dashboard/README.md`) serves a GPU-rendered stream and
applies the BT.709 rewrite on the encoder output.

## SDK pieces

| Header | Purpose |
| --- | --- |
| `amdva/vui.hpp` | `signal_bt709()` for H.264 access units |
| `amdva/rtsp_server.hpp` | `RtspServer`: mounts, appsrc factories, client/session shutdown |
| `amdva/egl_import.hpp` | `EglDmaBufImporter`: VA surface export to EGLImage/GL texture |
| `amdva/copy_audit.hpp` | weak hooks of the copy auditor |

Link `AMDVA::amdva_media`.
