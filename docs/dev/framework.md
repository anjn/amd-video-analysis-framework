# Video analytics framework

The AMD framework covers the main video analytics pipeline: decode, image/ROI
preprocessing, inference, detection/classification/segmentation/pose
postprocessing, tracking, metadata, drawing, Python callbacks and result
publication, plus hardware encoding. It vendors a subset of DL Streamer
(`third_party/dlstreamer`, MIT) for tracking, metadata conversion and stream
muxing rather than renaming AMD inference to `gvadetect` or adding a MIGraphX
backend to upstream OpenVINO inference. Existing DL Streamer `model-proc` files
and every upstream model adapter are not drop-in compatible. Developers coming
from DL Streamer or DeepStream should start with the
[DL Streamer migration guide](migration-from-dlstreamer.md) or the
[DeepStream migration guide](migration-from-deepstream.md). Audio, generative AI, 3D sensor fusion and
NPU execution are outside this implementation.

## Components

| Library | Public headers | Responsibility |
| --- | --- | --- |
| `amdva_runtime` | `runtime.hpp`, `preprocess.hpp`, `telemetry.hpp` | VA/HIP import, VPE operations, GPU image packing, inverse image geometry |
| `amdva_inference` | `model.hpp`, `batching.hpp` | Named static GPU tensor I/O, model manifests, task adapters, bounded shared batches |
| `amdva_gst_support` | `tensor_meta.hpp`, `video_preprocess.hpp`, `tracking.hpp`, `overlay.hpp`, `publishing.hpp`, `gst_telemetry.hpp` | GstAnalytics interop, GPU payload ownership, video preprocessing, imageless tracking, drawing and output |
| `amdva_media` | `vui.hpp`, `rtsp_server.hpp`, `egl_import.hpp` | BT.709 H.264 VUI signalling, appsrc RTSP server wrapper, Qt-free VA-to-EGL DMA-BUF texture import |
| `amdva_streaming` | `streaming/*.hpp` | RTSP access-unit ingest, compressed segment recording, SQLite recording/analysis catalog with storage quota, keyframe inspection, hardware playback and GPU preview surfaces |
| `amdva_monitor` | `resource_monitor.hpp` | CPU/GPU/memory/power readings and DRM engine utilization |

`amdva_streaming` has no Qt dependency. It provides building blocks, not an
application: camera scheduling, AI admission and search policies belong to the
application. Applications under [`examples/`](../../examples) are built only on the
public headers and `AMDVA::` targets:

| Example | Shows |
| --- | --- |
| [`examples/nvr`](nvr/README.md) | 16-camera NVR: a Qt-free controller (`amdva_nvr_backend`) on top of `amdva_streaming`, person search, the headless `amdva-streams` service and the Qt desktop |
| [`examples/resource-dashboard`](resource-dashboard/README.md) | GPU-rendered, hardware-encoded RTSP dashboard using `amdva_monitor` |
| [`examples/rtsp-files`](rtsp-files/README.md) | File-to-RTSP test server |
| [`examples/sdk`](../../examples/sdk) | Small programs for transcode, telemetry and recorded-video APIs |
| [`examples/pipelines`](../../examples/pipelines/README.md) | `gst-launch` recipes |

The NVR sample's image packer, batch executor, tracking and publication use the
same framework implementations as the plugins.

## Elements and supported tasks

| Element | Supported operation |
| --- | --- |
| `amddetect` | YOLOv8 detection `[B,4+C,N]`, instance segmentation with `[B,M,H,W]` prototypes, or pose with K x 3 keypoint channels |
| `amdclassify` | Classification logits `[B,C]`, softmax/top-k; defaults to `inference-region=roi-list` |
| `amdinference` | Full-frame execution of the task selected by model-info, including semantic logits `[B,C,H,W]` or `task_type=raw` GPU tensors |
| `amdwatermark` | GPU boxes, class labels, tracking IDs, masks and COCO-17 skeletons; NV12 VAMemory input, BGRA DMA-BUF output |
| `amdpython` | Metadata-only `callback(frame_dict)` extension, without exposing pixels or GPU addresses |
| `amdmetapublish` | Bounded asynchronous JSON-lines, MQTT QoS1 or Kafka publication |
| `amdencode` | VA hardware H.264/H.265/AV1 encoder bin for VAMemory or BGRA DMA-BUF frames; see [encoding and serving](encode-and-serve.md) |
| `amdpreproc`, `amdmigraphx` | Existing two-element YOLOv8 interface, retained for compatibility |

The vendored DL Streamer subset provides `gvatrack`, `gvametaconvert`,
`gvametapublish` (file, MQTT and Kafka modules), `gvastreammux`,
`gvastreamdemux` and `gvametaaggregate`. Other upstream elements
(`gvadetect`, `gvaclassify`, `gvainference`, `gvawatermark`, `gvapython`,
`gvaattachroi`, `gvaanalytics`, `gvafpscounter`, `gvafpsthrottle`) are not
included; use the `amd*` elements. See the [element reference](elements.md) for
every property.

Imageless `gvatrack` avoids CPU image mapping. Upstream image-based tracker and
watermark paths do not satisfy the AMD GPU memory contract and are not enabled
as substitutes. The selected-source NVR tracks continuously and draws IDs;
background keyframe results are independent observations and have no tracking ID.

## Prepare representative models

Run the exporter in its separate CPU environment. It uses torchvision's
ResNet18 ImageNet V1 weights, Ultralytics v8.3.0 YOLOv8n segmentation/pose weights,
and a pinned revision of NVIDIA's SegFormer-B0 ADE20K model. Export records
weight/source identities and ONNX SHA256 hashes. ResNet18 uses direct resize
and ImageNet normalization here; this is not torchvision's resize-plus-center-
crop evaluation recipe.

```bash
source scripts/env.sh
python3 scripts/setup_model_export.py
.deps/model-export/bin/python scripts/export_analytics_models.py artifacts/models
for name in resnet18 yolov8n-seg yolov8n-pose segformer-b0; do
  build/src/gst/amdva-compile-model artifacts/models/$name.onnx \
    artifacts/models/$name.mxr --model-info=artifacts/models/$name.json
done
```

Model weights are external artifacts and are not included under this repository's
MIT license. Review the [torchvision weights documentation](https://docs.pytorch.org/vision/stable/models/generated/torchvision.models.resnet18.html),
[Ultralytics licensing](https://www.ultralytics.com/license), and
[NVIDIA model card/license](https://huggingface.co/nvidia/segformer-b0-finetuned-ade-512-512)
for the intended deployment. The exporter records the SegFormer repository
revision rather than resolving a moving `main` branch.

Model-info describes image preprocessing and the output adapter:

```json
{
  "task_type": "classification",
  "adapter": "classification",
  "input": {
    "name": "images", "shape": [1, 3, 224, 224],
    "format": "NCHW", "channel_order": "RGB", "letterbox": false,
    "scale": 0.00392156862745098,
    "mean": [0.485, 0.456, 0.406], "std": [0.229, 0.224, 0.225]
  },
  "output": {"num_classes": 1000, "top_k": 5},
  "labels": "resnet18.txt"
}
```

Normalization is `(pixel * scale - mean) / std`. Default RGB scaling is
1/255, mean 0, std 1 and padding 114. Label paths are relative to model-info.
Supported task names are `detection`, `classification`, `instance-segmentation`,
`semantic-segmentation`, `pose`, and `raw`. YOLO adapters expect decoded
center/size boxes with class probabilities and no separate objectness channel;
YOLOv5, SSD, arbitrary model-proc converters and dynamic tensor shapes are not
supported by these adapters.

The compiler accepts `--batch=1|2|4|8|16`, `--precision=fp16|fp32`,
`--model-info=FILE`, and repeated `--input-shape=NAME:D1,D2,...`. The C++
`ModelExecutor` accepts multiple named static inputs and returns owned GPU
outputs. Its supported external types are float32/float16/int32/int64/uint8.
The automatic image elements and batch executor deliberately require one
float32 NCHW image input; supply multi-input tensors through the C++ API.
Postprocessors require float32 external outputs. FP16 compilation changes
internal computation while preserving the original external types.

## GStreamer pipelines

Set `VIDEO`, `DETECT`, `INFO` and `MODELS` to local paths after sourcing the
runtime environment. The detector's model and model-info are the same artifacts
used by the NVR. New task elements perform preprocessing internally; do not
insert the legacy `amdpreproc` in front of them.

```bash
gst-launch-1.0 -e filesrc location="$VIDEO" ! h264parse ! vah264dec ! \
  'video/x-raw(memory:VAMemory),format=NV12' ! \
  amddetect model="$DETECT" model-info="$INFO" inference-interval=5 ! \
  gvatrack tracking-type=short-term-imageless ! \
  amdclassify model="$MODELS/resnet18.mxr" model-info="$MODELS/resnet18.json" ! \
  amdpython module="$PWD/examples/metadata_callback.py" ! \
  amdmetapublish method=file file-path=events.jsonl ! \
  amdwatermark ! fakesink sync=false
```

Replace the classify/detect chain with either of these task elements:

```text
amddetect model=.../yolov8n-seg.mxr model-info=.../yolov8n-seg.json
amddetect model=.../yolov8n-pose.mxr model-info=.../yolov8n-pose.json
amdinference model=.../segformer-b0.mxr model-info=.../segformer-b0.json
```

For standalone classification set `inference-region=full-frame`. ROI mode reads
up to 300 standard GstAnalytics object boxes, crops on the VPE and relates the
classification to its parent object. ROI coordinates expand outward to NV12
chroma boundaries; the attachment records the actual crop. Segmentation masks
remain GPU tensors; their JSON describes storage and shape, not mask pixels.
Generic raw inference likewise keeps output tensors on the GPU.

All inference properties are mutable only in NULL/READY. `device` defaults to 0,
`batch-size` to 1, `batch-timeout` to 20 ms, `inference-interval` to 1,
`confidence-threshold` to .25 and `nms-threshold` to .45. Strict GPU mode is
mandatory. Compile a model for the selected batch size before starting.

## Shared batches and multiple streams

The same nonempty `model-instance-id` shares an executor across elements with
identical model paths, metadata, device, batch size, queue budget and thresholds.
Different settings under that ID are rejected. The worker admits at most one
request per source per batch, waits up to `batch-timeout`, and pads partial
batches on the GPU. GPU-to-GPU assembly is permitted; no CPU image or tensor
staging is introduced. ROI requests are separate child sources. Queue overflow
is an explicit error, not unbounded buffering. Source generations cancel queued
jobs and suppress stale in-flight results; GPU kernels already running complete
before their storage is released.

For several concurrent pipeline branches, give each inference element the same
ID and compiled batch size. For heterogeneous resolutions, `gvastreammux
output-mode=container` carries each source's original caps and memory. The AMD
inference element preprocesses each child with its own VA context, batches its
GPU tensor, and returns it to `gvastreamdemux`:

```text
source0 ! decode ! video/x-raw(memory:VAMemory),format=NV12 ! mux.sink_0
source1 ! decode ! video/x-raw(memory:VAMemory),format=NV12 ! mux.sink_1
gvastreammux name=mux output-mode=container !
  amddetect model=... model-info=... batch-size=4 ! gvastreamdemux name=demux
demux.src_0 ! queue ! gvatrack tracking-type=short-term-imageless ! amdwatermark ! sink0
demux.src_1 ! queue ! gvatrack tracking-type=short-term-imageless ! amdwatermark ! sink1
```

Each demux branch needs its own queue so sink preroll/backpressure cannot block
all sources. The selected mux patch preserves original stream-start identities
inside the container. The AMD elements accept video-only containers with up to
256 tracked source indices, not arbitrary lidar/audio objects.

## Metadata, callbacks and output

Standard GstAnalytics carries objects, classifications, tracking relations and
keypoint groups. `AmdAnalysisMeta` adds GPU masks/raw tensors, inverse geometry
and source/generation/sequence/PTS identity. Copies clone mutable attachment
lists and retain shared GPU storage; buffers may outlive their producing model
or pipeline. `gvametaaggregate` uses the selected patch to retain GPU payloads
and remap ROI parent IDs while merging branches. Region copies of GPU tensor
metadata are rejected rather than incorrectly reinterpreted.

`amdpython` calls `process(frame_dict)` by default. Return `None` or `True` to
pass, `False` to drop, or a JSON-serializable dict to attach custom metadata.
Exceptions become bus errors. Custom JSON is limited to 64 KiB. The callback
runs on the streaming thread under the Python GIL, so slow callbacks reduce
throughput. It can inspect compact detections/classes/keypoints and tensor
shapes; pixel access and device pointers are intentionally absent.

`amdmetapublish method=mqtt address=tcp://127.0.0.1:1883 topic=vision/events`
and `method=kafka address=127.0.0.1:9092 topic=vision-events` use the same
publisher as the NVR. The default is `none`. File publication appends JSON lines.
The SDK's `PublicationOptions` sets record and byte budgets (defaults 256 and
4 MiB); one record is limited to 64 KiB. Full queues discard oldest waiting
records, broker failures update status, and retries run independently of video.
MQTT uses QoS1; Kafka uses delivery callbacks. These are bounded best-effort
outputs, not durable or exactly-once event storage. MQTT/Kafka clients also have
bounded internal queues. Read `delivered`, `dropped`, `errors` while the element
is running, or use the SDK's `PublicationStatus` and `flush(timeout_ms)`.

JSON schema 1 includes source, generation, sequence, PTS, background flag,
objects with boxes/classes/tracking/keypoints, full-frame classifications,
GPU tensor descriptors with data type, device and batch index, and optional custom
metadata. It never serializes image buffers, masks or GPU pointers. Source URLs
are not added automatically; applications choose the source identity.

## Headless build and SDK

```bash
source scripts/env.sh
cmake -S . -B build-headless -DAMDVA_BUILD_NVR_GUI=OFF \
  -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT"
cmake --build build-headless -j6
build-headless/examples/nvr/amdva-streams examples/nvr/config/nvr.json 60
cmake --install build-headless --prefix /path/to/amdva-sdk
```

`-DAMDVA_BUILD_NVR_GUI=OFF` keeps the NVR backend and its headless
`amdva-streams` service but needs no Qt; `-DAMDVA_BUILD_EXAMPLES=OFF` builds the
framework only. `amdva-streams` receives/records sources and performs
selected/background analytics without allocating EGL previews or requiring a
display server. A zero duration or omitted duration runs until SIGINT/SIGTERM.

External CMake consumers use `find_package(AMDVideoAnalytics REQUIRED)` and
link `AMDVA::amdva_streaming`, `AMDVA::amdva_gst_support`,
`AMDVA::amdva_inference`, `AMDVA::amdva_media`, `AMDVA::amdva_monitor` or
`AMDVA::amdva_runtime`. The same targets exist in the source tree, so every
directory under `examples/` also configures on its own against an installed SDK
(see [`examples/sdk/CMakeLists.txt`](../../examples/sdk/CMakeLists.txt)). The installed package
requires the compatible host VA/GStreamer/ROCm stack; it does not bundle a
portable driver runtime. Set `AMDVA_ROCM_ROOT` when locating HIP headers on a
compatible installation.

## Decode and presentation telemetry

The Qt-independent telemetry collector and GStreamer adapters measure local
RTP receipt to decoder output and renderer notifications, with rolling
percentiles, frame rates, update gaps and known local discard counters.
NVR and headless commands publish statistics separately from AI result JSON.
No latency/FPS acceptance thresholds or automatic controls are imposed. See
[timing boundaries, SDK adapters and validation](telemetry.md).


Recording queries, protected playback handles, the Qt-free recording player and
the recorded-analysis store are part of `amdva_streaming`. Applications keep
their own tables in the catalog database with `Catalog::extend` (the NVR's
person-search index is one). See the [VMS interfaces and migration](nvr/vms.md#framework-interfaces-and-migration)
and [SDK playback example](../../examples/sdk/playback.cpp).

The Qt-free `amdva_monitor` library provides optional-valued CPU/GPU/memory/power
readings and observable DRM engine utilization via `amdva/resource_monitor.hpp`.
The [resource dashboard sample](resource-dashboard/README.md) uses it independently of
NVR, analytics plugins and model loading.
