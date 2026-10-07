# Video analytics framework

The AMD framework covers the main video analytics pipeline: decode, image/ROI
preprocessing, inference, detection/classification/segmentation/pose
postprocessing, tracking, metadata, drawing, Python callbacks and result
publication. It reuses selected DL Streamer elements rather than renaming AMD
inference to `gvadetect` or adding a MIGraphX backend to upstream OpenVINO
inference. Existing DL Streamer `model-proc` files and every upstream model
adapter are not drop-in compatible. Audio, generative AI, 3D sensor fusion and
NPU execution are outside this implementation.

## Components

| Library | Public headers | Responsibility |
| --- | --- | --- |
| `amdva_runtime` | `runtime.hpp`, `preprocess.hpp`, `telemetry.hpp` | VA/HIP import, VPE operations, GPU image packing, inverse image geometry |
| `amdva_inference` | `model.hpp`, `batching.hpp` | Named static GPU tensor I/O, model manifests, task adapters, bounded shared batches |
| `amdva_gst_support` | `tensor_meta.hpp`, `video_preprocess.hpp`, `tracking.hpp`, `overlay.hpp`, `publishing.hpp`, `gst_telemetry.hpp` | GstAnalytics interop, GPU payload ownership, video preprocessing, imageless tracking, drawing and output |
| `amdva_streaming` | `streaming/*.hpp` | Dynamic RTSP registration/selection, reconnect, compressed recording, SQLite catalog, storage quota, background keyframe admission and analytics scheduling |
| Desktop NVR | `src/nvr` | Qt controls and EGL tile presentation; uses the public streaming library |

`amdva_streaming` has no Qt dependency. Applications can use its controller
without a desktop, or construct ordinary GStreamer pipelines with the AMD
plugins. The NVR retains its latest-frame admission policy and preview buffers;
its image packer, batch executor, tracking and publication now use the same
framework implementations as the plugins. No inference implementation remains
specific to the Qt UI.

## Elements and supported tasks

| Element | Supported operation |
| --- | --- |
| `amddetect` | YOLOv8 detection `[B,4+C,N]`, instance segmentation with `[B,M,H,W]` prototypes, or pose with K x 3 keypoint channels |
| `amdclassify` | Classification logits `[B,C]`, softmax/top-k; defaults to `inference-region=roi-list` |
| `amdinference` | Full-frame execution of the task selected by model-info, including semantic logits `[B,C,H,W]` or `task_type=raw` GPU tensors |
| `amdwatermark` | GPU boxes, class labels, tracking IDs, masks and COCO-17 skeletons; NV12 VAMemory input, BGRA DMA-BUF output |
| `amdpython` | Metadata-only `callback(frame_dict)` extension, without exposing pixels or GPU addresses |
| `amdmetapublish` | Bounded asynchronous JSON-lines, MQTT QoS1 or Kafka publication |
| `amdpreproc`, `amdmigraphx` | Existing two-element YOLOv8 interface, retained for compatibility |

Selected upstream elements include `gvatrack`, `gvametaconvert`,
`gvametapublish`, `gvastreammux`, `gvastreamdemux`, `gvametaaggregate`,
`gvaattachroi`, `gvaanalytics`, `gvafpscounter` and `gvafpsthrottle`.
Upstream MQTT/Kafka publisher modules and `gvapython` are also built. Upstream
`gvapython` additionally requires a working PyGObject environment and the
GStreamer Video/Analytics typelibs; the rootless baseline does not supply those
typelibs. Use `amdpython` for the validated metadata callback interface.

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

## Headless SDK

```bash
source scripts/env.sh
cmake -S . -B build-headless -DAMDVA_BUILD_NVR=OFF \
  -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT"
cmake --build build-headless -j6
build-headless/src/streaming/amdva-streams examples/nvr.json 60
cmake --install build-headless --prefix /path/to/amdva-sdk
```

`amdva-streams` receives/records sources and performs selected/background
analytics without allocating EGL previews or requiring a display server. A
zero duration or omitted duration runs until SIGINT/SIGTERM. Use the public
controller's `add`, `remove` and `select` methods for dynamic control; commands
are serialized on its control worker. `status` and `publication_status` provide
snapshots. See [the SDK example](../examples/sdk/CMakeLists.txt).

External CMake consumers use `find_package(AMDVideoAnalysis REQUIRED)` and
link `AMDVA::amdva_streaming`, `AMDVA::amdva_gst_support`,
`AMDVA::amdva_inference` or `AMDVA::amdva_runtime`. The installed package
requires the compatible host VA/GStreamer/ROCm stack; it does not bundle a
portable driver runtime. Set `AMDVA_ROCM_ROOT` when locating HIP headers on a
compatible installation. The Qt desktop can be disabled independently of the
streaming framework.

## Decode and presentation telemetry

The Qt-independent telemetry collector and GStreamer adapters measure local
RTP receipt to decoder output and renderer notifications, with rolling
percentiles, frame rates, update gaps and known local discard counters.
NVR and headless commands publish statistics separately from AI result JSON.
No latency/FPS acceptance thresholds or automatic controls are imposed. See
[timing boundaries, SDK adapters and validation](telemetry.md).
