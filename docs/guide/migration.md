<!-- source: docs/guide/migration.ja.md bac360f54c0de0b02dbc8fce97cac7802605e427 -->
# Migrating from other frameworks

Intel DL Streamer and NVIDIA DeepStream pipelines can run on this framework by replacing elements. All of them use GStreamer and GstAnalytics metadata; what changes is the inference elements, the model format and the memory where frames live. This page is a quick reference of the correspondences; property-level mappings are in [Migrating from DL Streamer](../dev/migration-from-dlstreamer.md) and [Migrating from DeepStream](../dev/migration-from-deepstream.md).

## Element mapping

| Task | DL Streamer | DeepStream | This framework |
| --- | --- | --- | --- |
| Decode | `decodebin3`, etc. | `nvv4l2decoder` | `vah264dec`, `vah265dec` |
| Detection | `gvadetect` | `nvinfer` (primary) | `amddetect` |
| Classification | `gvaclassify` | `nvinfer` (secondary) | `amdclassify` |
| Other inference | `gvainference` | `nvinfer` | `amdinference` |
| Tracking | `gvatrack` | `nvtracker` | `gvatrack` (bundled, imageless modes only) |
| Draw | `gvawatermark` | `nvdsosd` | `amdwatermark` |
| Encode | `vah264enc`, etc. | `nvv4l2h264enc` | `amdencode` |
| Python | `gvapython` | `pyds` probes | `amdpython` (metadata only) |
| Publish results | `gvametaconvert` + `gvametapublish` | `nvmsgconv` + `nvmsgbroker` | `amdmetapublish`, or the bundled `gvametapublish` |
| Multiple streams | `gvastreammux` | `nvstreammux` | `gvastreammux output-mode=container` (bundled) |

## What changes when migrating

| Item | Before | This framework |
| --- | --- | --- |
| Model | OpenVINO IR, TensorRT engine | `.mxr` compiled from ONNX with `amdva-compile-model` |
| Preprocessing and output configuration | `model-proc`, `config_infer_*.txt` | `model-info.json` ([reference](../dev/model-info.md)) |
| Frame memory | System memory, NVMM | NV12 on the GPU (`video/x-raw(memory:VAMemory),format=NV12`) |
| Format conversion | `videoconvert`, `nvvideoconvert` | Not needed. Inference, drawing and encoding are linked on the GPU |
| Batch size | Specified at runtime | Fixed at compile time; set the element's `batch-size` to the same value |
| Property changes | Some can be changed while running | `amd*` elements can be changed only in the NULL/READY states |

Supported models are YOLOv8-family detection, segmentation and pose estimation, classification, semantic segmentation and raw tensor output. YOLOv5, SSD and custom postprocessing libraries are not supported.

## Example pipeline rewrite

Rewrite a DeepStream detection, tracking, drawing and encoding pipeline as follows.

```bash
# DeepStream
gst-launch-1.0 uridecodebin uri=file:///in.mp4 ! m.sink_0 \
  nvstreammux name=m batch-size=1 width=1920 height=1080 ! \
  nvinfer config-file-path=config_infer_primary.txt ! \
  nvtracker ll-lib-file=libnvds_nvmultiobjecttracker.so ll-config-file=tracker.yml ! \
  nvvideoconvert ! nvdsosd ! nvvideoconvert ! nvv4l2h264enc ! h264parse ! mp4mux ! filesink location=out.mp4

# This framework
gst-launch-1.0 -e filesrc location=in.mp4 ! qtdemux ! h264parse ! vah264dec ! \
  'video/x-raw(memory:VAMemory),format=NV12' ! \
  amddetect model=model.mxr model-info=model-info.json ! \
  gvatrack tracking-type=short-term-imageless ! \
  amdwatermark ! amdencode ! h264parse ! mp4mux ! filesink location=out.mp4
```

With a single input, the stream-batching element is not needed. For how to combine multiple inputs, see [Building pipelines](pipelines.md).
