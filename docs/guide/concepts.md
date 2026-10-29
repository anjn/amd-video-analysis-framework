<!-- source: docs/guide/concepts.ja.md 3115d739d30e0aab76fe4a92e8fc12db1e0a7e82 -->
# Concepts

This framework keeps decoded video in GPU memory all the way through processing. In the framework's elements, the CPU handles only compressed video data and small metadata such as detection results; it never copies or converts pixels. Applications may add copies for their own processing; for example, person search in the [NVR demo](nvr-demo.md) reads cropped person images back to the CPU. This page describes the target hardware, the data flow, the roles of the elements, models, metadata and the library structure.

## Ryzen AI APU hardware

This framework targets AMD Ryzen AI APUs. An APU is a processor that combines several kinds of compute units on one chip. The Ryzen AI 9 HX 370 used for validation has the following five.

| Unit | On the validation machine | Good at | Used in this framework for |
| --- | --- | --- | --- |
| CPU | Zen 5 and Zen 5c, 12 cores, 24 threads | General-purpose processing | Network reception, writing recordings, metadata processing, application control |
| GPU | Radeon 890M (gfx1150) | Massively parallel computation and rendering | Pre- and postprocessing with HIP, inference with MIGraphX, display with OpenGL ES |
| NPU | XDNA 2 | Neural network inference at low power | Embedding computation for person search in the NVR demo, through the Vitis AI execution provider of ONNX Runtime |
| VCN | VCN 4.0.5 | Video encoding and decoding | H.264/H.265/AV1 decoding and encoding, through VA-API |
| VPE | VPE 6.1.0 | Video scaling, color space conversion and cropping | Conversion to the inference input size, scaling and color conversion for display, through VA-API video processing |

VCN (Video Core Next) and VPE (Video Processing Engine) are dedicated video circuits. VPE converts the NV12 video that the decoder outputs into the BGRA format used for inference and display, scaling it to the required size at the same time. GPU shaders can do the same work, but leaving it to VPE keeps the GPU free for inference and rendering. The framework checks at startup that VPE actually ran (see the check in [Installation](install.md)) and never falls back to GPU shaders or the CPU when VPE cannot do the work.

### Shared memory and zero-copy

In an APU, all compute units share the same main memory (DRAM). Using a mechanism called DMA-BUF, a decoded video buffer can be handed between VCN, VPE and the GPU (HIP, OpenGL ES) as the same memory. Handing data over without duplicating it in this way is called "zero-copy".

Duplicating (copying) data between compute units, on the other hand, uses memory bandwidth and time even though it stays within the same DRAM. One 2560×1440 frame is about 5.5 MB (NV12), so copying sixteen 30 fps cameras would move 2.6 GB per second. Avoiding these copies is key to performance with many cameras.

## Data flow

```mermaid
flowchart TB
  IN[Compressed video<br>RTSP, files] --> DEC
  subgraph GPU[GPU]
    DEC[VA decode<br>NV12] --> PRE[VPE<br>crop and scale] --> NORM[HIP<br>normalize into tensors] --> INF[MIGraphX<br>inference] --> POST[Postprocessing<br>NMS etc.]
    DEC --> DRAW[Draw] --> ENC[VA encode]
  end
  POST --> META[Metadata<br>boxes, classes, IDs]
  META -.draw commands.-> DRAW
  META --> JSON[JSON, MQTT, Kafka]
  ENC --> OUT[Compressed video<br>MP4, RTSP]
```

| Stage | Hardware | What it does |
| --- | --- | --- |
| Decode, encode | VCN (VA-API) | H.264/H.265 decoding and H.264/H.265/AV1 encoding |
| Crop, scale | VPE (VA video processing) | Conversion to the inference input size and cropping of regions (ROIs) for classification |
| Normalize | GPU (HIP) | Converts pixels into the model input tensor (float32) |
| Inference, postprocessing | GPU (MIGraphX, HIP) | Inference, bounding box filtering (NMS), mask and keypoint reconstruction |
| Draw | GPU (VPE, HIP) | Draws bounding boxes, labels, IDs, masks and skeletons |

Segmentation masks and raw inference outputs stay on the GPU as tensors. Only bounded results such as box coordinates and classes are read back to the CPU.

## Element roles

You build a pipeline by linking GStreamer elements. This framework provides the following elements.

| Element | Role |
| --- | --- |
| `amddetect` | Object detection, instance segmentation, pose estimation (YOLOv8 family) |
| `amdclassify` | Classification per detected object or of the whole frame |
| `amdinference` | Inference with arbitrary models, such as semantic segmentation |
| `amdwatermark` | Draws detection results |
| `amdencode` | Hardware encoding |
| `amdpython` | Calls a Python function that receives detection results (pixels are not passed) |
| `amdmetapublish` | Publishes detection results to JSON Lines files, MQTT or Kafka |
| `gvatrack` and other `gva*` | Tracking, multi-stream batching and metadata conversion bundled from Intel DL Streamer |

Inference elements accept only NV12 on the GPU (`video/x-raw(memory:VAMemory),format=NV12`) as input. They never silently copy frames from the CPU; if elements are linked incorrectly, the pipeline fails at startup.

## Models

Compile an ONNX model for this GPU with `amdva-compile-model` and use the resulting `.mxr` file. Preprocessing and output format are described in a `model-info.json` placed next to the model.

- The batch size is 1, 2, 4, 8 or 16 and is fixed at compile time.
- Compilation also creates a manifest that records the hash and the GPU; a model compiled for a different GPU is rejected at load time.
- Supported output formats are YOLOv8-family detection, segmentation and pose estimation, classification, semantic segmentation and raw tensors.

For the fields of `model-info.json`, see the [model-info reference](../dev/model-info.md).

## Metadata

Detection results are attached to frames as standard GStreamer GstAnalytics metadata. Downstream elements and your own probes can read them the same way as with DL Streamer. GPU masks and tensors, and frame identification information, go into this framework's own metadata.

`amdmetapublish` and `amdpython` handle the same JSON format (schema 1). Each frame is one record, and each object has a class, confidence, box, tracking ID, classifications and keypoints. For details of the format, see [Metadata and Python](../dev/metadata-and-python.md).

## Library structure

From an application, link the libraries in the CMake package `AMDVideoAnalytics`. Arrows show which library uses which.

```mermaid
flowchart BT
  runtime[amdva_runtime<br>VA/HIP interop, preprocessing]
  inference[amdva_inference<br>models, batch execution]
  gst[amdva_gst_support<br>metadata, tracking, drawing, publishing]
  streaming[amdva_streaming<br>RTSP ingest, recording, catalog, playback]
  media[amdva_media<br>encoding helpers, RTSP serving, EGL]
  monitor[amdva_monitor<br>CPU, GPU and power monitoring]
  inference --> runtime
  gst --> inference
  streaming --> gst
```

`amdva_media` and `amdva_monitor` can be used without depending on the other libraries. Usage is described in [Embedding the SDK in your application](sdk.md).
