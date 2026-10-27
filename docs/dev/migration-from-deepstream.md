# Coming from NVIDIA DeepStream

DeepStream and this framework are both GStreamer pipelines with a metadata
model. The main differences: inference runs through MIGraphX on AMD GPUs
instead of TensorRT; frames live in VA surfaces (`memory:VAMemory`) instead of
NVMM; and per-model settings are a small JSON file (`model-info`) instead of a
`config_infer_*.txt`.

## Element map

| DeepStream | This framework | Notes |
| --- | --- | --- |
| `nvv4l2decoder` | `vah264dec` / `vah265dec` / `vaav1dec` | Output `video/x-raw(memory:VAMemory),format=NV12` |
| `nvstreammux` | `gvastreammux output-mode=container`, or independent branches with a shared `model-instance-id` | Container mode keeps each source's resolution; there is no `width`/`height` rescale to a common batch size. |
| `nvinfer` (primary) | `amddetect` / `amdinference` | |
| `nvinfer` (secondary, `process-mode=2`) | `amdclassify` | `inference-region=roi-list` classifies boxes from the upstream detector (up to 300) |
| `nvtracker` | `gvatrack tracking-type=short-term-imageless` | NvDCF, IOU and DeepSORT low-level libraries are not available. An image-free IOU/Kalman tracker only. |
| `nvdsosd` | `amdwatermark` | Boxes, labels, track IDs, masks, COCO-17 skeletons on the GPU |
| `nvvideoconvert` | none needed; `vapostproc` for explicit format/scale | `amd*` elements take NV12 VAMemory directly |
| `nvmsgconv` + `nvmsgbroker` | `amdmetapublish` (MQTT, Kafka, file) | Different JSON schema; see [metadata](metadata-and-python.md). AMQP, Azure IoT and Redis are not provided. |
| `nvdsanalytics` (ROI, line crossing, direction) | not provided | Implement on the metadata in `amdpython` or a probe. |
| `nvdspreprocess` | built into the inference elements | VA video processor crop/scale + HIP normalization |
| `nvv4l2h264enc` / `nvv4l2h265enc` | `amdencode` | See [encoding and serving](encode-and-serve.md) |
| `nvstreamdemux` | `gvastreamdemux` | |
| `nvdsmetamux`, `nvof`, `nvdspostprocess` | not provided | |

## `nvinfer` configuration

| `config_infer_*.txt` / property | Here |
| --- | --- |
| `onnx-file`, `model-engine-file` | `model` = `.mxr` from `amdva-compile-model model.onnx model.mxr`. TensorRT engines cannot be imported. |
| `net-scale-factor`, `offsets` | `input.scale`, `input.mean` (`offset * scale`) in model-info |
| `model-color-format` | `input.channel_order` |
| `maintain-aspect-ratio`, `symmetric-padding` | `input.letterbox` (symmetric padding is what letterbox does) |
| `infer-dims` | `input.shape` |
| `num-detected-classes`, `labelfile-path` | `output.num_classes`, `labels` |
| `network-type` (0 detector, 1 classifier, 2 segmentation) | `task_type` and element choice |
| `batch-size` | element `batch-size` = compiled batch (`--batch`) |
| `interval` (frames **skipped**) | `inference-interval` (infer **every Nth** frame): `interval=2` becomes `inference-interval=3` |
| `gie-unique-id`, `operate-on-gie-id` | pipeline order; `model-instance-id` only shares one executor |
| `operate-on-class-ids`, `filter-out-class-ids` | not available; filter in a callback |
| `pre-cluster-threshold`, `nms-iou-threshold`, `cluster-mode` | `confidence-threshold`, `nms-threshold` (NMS only) |
| `parse-bbox-func-name`, `custom-lib-path` | not available. Adapters: `yolov8`, `yolov8-seg`, `yolov8-pose`, classification, semantic segmentation, raw tensors |
| `network-mode` (FP32/FP16/INT8) | `amdva-compile-model --precision=fp16|fp32`; no INT8 path |
| `output-tensor-meta` | `task_type=raw` keeps output tensors on the GPU in `AmdAnalysisMeta` |
| `tensor-meta-pool-size`, `workspace-size` | none |

Example, DeepStream:

```bash
gst-launch-1.0 uridecodebin uri=file:///in.mp4 ! m.sink_0 \
  nvstreammux name=m batch-size=1 width=1920 height=1080 ! \
  nvinfer config-file-path=config_infer_primary.txt ! \
  nvtracker ll-lib-file=libnvds_nvmultiobjecttracker.so ll-config-file=tracker.yml ! \
  nvvideoconvert ! nvdsosd ! nvvideoconvert ! nvv4l2h264enc ! h264parse ! mp4mux ! filesink location=out.mp4
```

This framework:

```bash
gst-launch-1.0 -e filesrc location=in.h264 ! h264parse ! vah264dec ! \
  'video/x-raw(memory:VAMemory),format=NV12' ! \
  amddetect model=model.mxr model-info=model-info.json batch-size=1 ! \
  gvatrack tracking-type=short-term-imageless ! \
  amdwatermark ! amdencode bitrate=6000 ! h264parse ! mp4mux ! filesink location=out.mp4
```

There is no `nvvideoconvert` because nothing leaves the GPU in between.

## Batching and multiple streams

DeepStream batches by pushing every source into `nvstreammux`. Here, give every
branch an inference element with the same `model-instance-id`, the same model
and the same `batch-size`; the shared executor assembles one batch per
`batch-timeout` (default 20 ms) from all branches, one request per source per
batch, padding partial batches on the GPU. For sources of different resolutions
use `gvastreammux output-mode=container ! amddetect ! gvastreamdemux` and give
every demux branch its own `queue`. See `examples/pipelines/multistream-encode.sh`.

`batched-push-timeout` and `live-source` have no direct property; for live RTSP
sources do not set `max-fps` on the mux.

## Metadata

| DeepStream | Here |
| --- | --- |
| `NvDsBatchMeta`, `NvDsFrameMeta` | one buffer per frame; frame identity (source, generation, sequence, PTS) in `AmdAnalysisMeta` |
| `NvDsObjectMeta` (`rect_params`, `confidence`, `class_id`, `object_id`) | `GstAnalyticsODMtd` (box, score, label quark) related to a `GstAnalyticsTrackingMtd` |
| classifier meta | `GstAnalyticsClsMtd` related to the parent object |
| mask / segmentation meta | GPU tensors in `AmdAnalysisMeta` |
| `NvDsUserMeta` tensor output | `task_type=raw`, tensors stay on the GPU |
| `pyds` probes | `amdpython` (`frame_dict`, read-only metadata) or a C++ pad probe |
| `pyds.get_nvds_buf_surface` (pixel access) | not available by design; use the C++ GPU APIs |

## Memory

| DeepStream | Here |
| --- | --- |
| `video/x-raw(memory:NVMM)` | `video/x-raw(memory:VAMemory)` (decoder, inference, encoder) |
| `nvbuf-memory-type` | none |
| CUDA unified memory | HIP device buffers owned by the model executor; VA surfaces imported into HIP via DMA-BUF |

The AMD inference elements only accept NV12 VAMemory (or a gvastreammux
container of it) and default to `require-zero-copy=true`, so caps negotiation
fails instead of silently copying system-memory frames.

## Not provided

INT8 calibration, TensorRT engine import, `nvinferserver`/Triton, NvDCF and
DeepSORT trackers, ReID tracker features, `nvdsanalytics`, custom bounding-box
parsers, audio and Smart Record. Person ReID and semantic search exist in the
[NVR sample](nvr/README.md), not as framework elements.
