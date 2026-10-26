# Coming from Intel DL Streamer

This guide maps a DL Streamer pipeline onto this framework. The pipeline
structure is the same (GStreamer elements, GstAnalytics metadata, `gva*` for
tracking/convert/publish); what changes is inference, which runs on the AMD GPU
through MIGraphX, and the memory model, which keeps frames on the GPU from the
decoder to the encoder.

## What is the same

- Plain GStreamer: `gst-launch-1.0`, `gst_parse_launch`, bins, caps negotiation.
- Metadata is stored as standard `GstAnalytics` relation metadata (object
  detection, classification, tracking and keypoint/group metadata) so
  downstream elements and your own probes read it the same way.
- `gvatrack`, `gvametaconvert`, `gvametapublish`, `gvastreammux`,
  `gvastreamdemux` and `gvametaaggregate` are the DL Streamer v2026.2.0 sources,
  vendored in `third_party/dlstreamer` and built by default
  (`AMDVA_BUILD_GVA_COMPAT`). Element and property names are unchanged.
- `inference-region`, `model-instance-id`, `batch-size`, `inference-interval`
  and `batch-timeout`-style properties keep their DL Streamer meaning.

## Element map

| DL Streamer | This framework | Notes |
| --- | --- | --- |
| `gvadetect` | `amddetect` | YOLOv8 family (detection, instance segmentation, pose). No OpenVINO IR, no `model-proc`. |
| `gvaclassify` | `amdclassify` | Defaults to `inference-region=roi-list`, classifying boxes from an upstream detector. |
| `gvainference` | `amdinference` | Full-frame; also semantic segmentation and raw GPU tensors (`task_type=raw`). |
| `gvatrack` | `gvatrack` (vendored) | Use `tracking-type=short-term-imageless` or `zero-term-imageless`. Image-based trackers are not enabled: they map frames on the CPU. |
| `gvawatermark` | `amdwatermark` | GPU overlay: input VAMemory NV12, output BGRA DMA-BUF. Follow it with `amdencode` for compressed output. |
| `gvapython` | `amdpython` | `process(frame_dict)` callback on metadata only; no pixel or `VideoFrame` access. |
| `gvametaconvert` | `gvametaconvert` (vendored) or `amdpython`/`amdmetapublish` JSON | |
| `gvametapublish` | `gvametapublish` (vendored) or `amdmetapublish` | `amdmetapublish` is bounded and never blocks video; see [metadata](metadata-and-python.md). |
| `gvastreammux` / `gvastreamdemux` | same (vendored) | `output-mode=container` carries each source's own caps and memory. |
| `gvametaaggregate` | same (vendored) | Patched to merge AMD GPU payloads (`amd-analytics-merge`). |
| `gvafpscounter`, `gvafpsthrottle`, `gvaattachroi`, `gvaanalytics` | not included | Use `identity`/`fpsdisplaysink`, or application code on the metadata. |
| `vah264enc` (GStreamer) | `amdencode` | Adds BGRA DMA-BUF input and BT.709 signalling; see [encoding and serving](encode-and-serve.md). |

## Properties

### `gvadetect` / `gvaclassify` / `gvainference`

| DL Streamer property | `amd*` equivalent |
| --- | --- |
| `model` (OpenVINO IR `.xml`) | `model` (MIGraphX `.mxr`, compiled with `amdva-compile-model`) |
| `model-proc` | `model-info` (JSON, see [model-info](model-info.md)) |
| `device=GPU` / `CPU` / `NPU` | `device` is a HIP device index (default 0); there is no CPU or NPU fallback |
| `batch-size` | `batch-size` (must equal the compiled batch size, 1-16) |
| `inference-interval` | `inference-interval` |
| `threshold` | `confidence-threshold` (detection), plus `nms-threshold` |
| `inference-region` | `inference-region` (`full-frame` or `roi-list`) |
| `model-instance-id` | `model-instance-id` (shared executor across elements) |
| `pre-process-backend` | none: preprocessing is always VA video processor + HIP, `require-zero-copy=true` |
| `nireq`, `ie-config`, `cpu-throughput-streams` | none; use `batch-size` and `batch-timeout` |
| `object-class`, `labels-file` | labels come from the `labels` file named in model-info |

### `gvametaconvert` / `gvametapublish`

Properties are unchanged because the elements are the DL Streamer sources. The
vendored build includes the file, MQTT and Kafka publishers.

## Example

DL Streamer:

```bash
gst-launch-1.0 filesrc location=in.mp4 ! decodebin3 ! \
  gvadetect model=yolov8n.xml model-proc=yolov8n.json device=GPU batch-size=1 ! \
  gvatrack tracking-type=short-term-imageless ! \
  gvawatermark ! videoconvert ! x264enc ! mp4mux ! filesink location=out.mp4
```

This framework:

```bash
gst-launch-1.0 -e filesrc location=in.h264 ! h264parse ! vah264dec ! \
  'video/x-raw(memory:VAMemory),format=NV12' ! \
  amddetect model=yolov8n.mxr model-info=yolov8n.json batch-size=1 ! \
  gvatrack tracking-type=short-term-imageless ! \
  amdwatermark ! amdencode bitrate=6000 ! h264parse ! mp4mux ! filesink location=out.mp4
```

Differences to expect:

- Decode with a VA decoder and constrain caps to `video/x-raw(memory:VAMemory)`;
  system-memory frames are not accepted by the AMD elements.
- No `videoconvert`: the frame never reaches system memory. `amdwatermark`
  outputs a GPU surface that `amdencode` encodes directly.
- Raw `.h264` files carry no timestamps; insert `video/x-h264,framerate=30/1`
  after `h264parse` (MP4/RTSP sources do not need it).
- Properties of the AMD elements are writable only in NULL/READY.

## Models

1. Export the model to ONNX (the `scripts/export_*.py` helpers show the steps for
   YOLOv8, ResNet18 and SegFormer).
2. Write a model-info JSON describing the input tensor and output layout
   (see [model-info](model-info.md)). This replaces `model-proc`.
3. Compile for the target GPU and batch size:
   `amdva-compile-model model.onnx model.mxr --batch=4 --model-info=model.json`.
   The compiler writes `model.mxr.manifest.json` with a SHA256 and the GPU
   architecture; the runtime rejects a missing or mismatched manifest.

OpenVINO IR files, `model-proc` converters and custom post-processing
libraries are not supported. Adapters available today: `yolov8`, `yolov8-seg`,
`yolov8-pose`, classification, semantic segmentation and raw tensors. YOLOv5 and
SSD are not supported.

## Metadata access

- **Python.** `gvapython` is not shipped. Use `amdpython` with
  `process(frame_dict)`; see [metadata and Python](metadata-and-python.md).
- **C/C++ probes.** Read `GstAnalyticsRelationMeta` as you do with DL Streamer.
  GPU-resident results (masks, raw tensors) are in `AmdAnalysisMeta`
  (`amdva/tensor_meta.hpp`); they are never copied to system memory implicitly.
- **GVA JSON meta / tensors.** `gvametaconvert` reads GstAnalytics, so it works
  with `amddetect` output.

## Multiple streams

`gvastreammux output-mode=container ! amddetect batch-size=N ! gvastreamdemux`
batches sources of different resolutions through one detector. Give each demux
branch its own `queue`. See [framework.md](framework.md#shared-batches-and-multiple-streams)
and `examples/pipelines/multistream-encode.sh`.

## Licensing

The vendored DL Streamer sources keep their MIT license and Intel copyright
(`third_party/dlstreamer/LICENSE`, `README.AMD.md` lists the local changes).
The GstAnalytics group/keypoint API comes from a patch to gst-plugins-bad that
also originates in DL Streamer (`patches/gst-plugins-bad-0020-gst-analytics.patch`,
LGPL-2.1-or-later files). See `THIRD_PARTY_LICENSES.md`.
