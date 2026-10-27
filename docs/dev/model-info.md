# Model-info reference

`model-info` is a small JSON file next to a compiled `.mxr` model. It replaces
DL Streamer's `model-proc` and DeepStream's `config_infer_*.txt` preprocessing
and post-processing fields. It is parsed by `ModelInfo::load`
(`include/amdva/model.hpp`, `src/model.cpp`) and shared by all `amd*` inference
elements and the SDK.

```json
{
  "task_type": "detection",
  "adapter": "yolov8",
  "input": {
    "name": "images",
    "shape": [1, 3, 640, 640],
    "format": "NCHW",
    "channel_order": "RGB",
    "letterbox": true,
    "scale": 0.00392156862,
    "mean": [0, 0, 0],
    "std": [1, 1, 1],
    "padding": 114
  },
  "output": { "format": "cxcywh", "num_classes": 80, "top_k": 5, "keypoints": 17 },
  "labels": "coco80.txt"
}
```

## Fields

| Field | Default | Meaning |
| --- | --- | --- |
| `task_type` | `detection` | `detection`, `instance-segmentation`, `pose`, `classification`, `semantic-segmentation` or `raw` |
| `adapter` (alias `family`) | `yolov8` for detection, segmentation and pose; otherwise the task name | Output decoder. Detection adapters: `yolov8`, `yolov8-seg`, `yolov8-pose`. YOLOv5, SSD and custom parsers are not supported. |
| `input.name` | `images` | Model input tensor name |
| `input.shape` | required | `[N,3,H,W]`: N 1-16, 3 channels, H and W 2-4096. Static shapes only. |
| `input.format` | `NCHW` | Only `NCHW` is accepted |
| `input.channel_order` | `RGB` | `RGB` or `BGR` |
| `input.letterbox` | `true`; `false` for classification and semantic segmentation | Preserve aspect ratio and pad, or resize directly |
| `input.scale`, `mean`, `std` | `1/255`, `[0,0,0]`, `[1,1,1]` | Applied as `(pixel * scale - mean) / std` per channel, in the channel order above. `std` must be positive. |
| `input.padding` | `114` | Letterbox pad value |
| `input.normalize` | none | Legacy; only `[0, 1]` is accepted. Use `scale`/`mean`/`std` instead. |
| `output.num_classes` | 80 | Class count, 1-65536. Must equal the number of lines in the labels file. |
| `output.top_k` | 5 | Classification results kept, 1-100 |
| `output.keypoints` | 17 | Keypoints per person for pose, 1-64 |
| `output.format` | informational | Decoded box layout of the model (`cxcywh` for YOLOv8) |
| `labels` | indices as strings | Text file, one label per line, relative to the JSON file |

YOLO adapters expect decoded center/size boxes with class probabilities and no
separate objectness channel (`[B, 4+C, N]`). Segmentation adds `[B,M,H,W]`
prototypes; pose adds K x 3 keypoint channels. Classification expects logits
`[B,C]`; semantic segmentation `[B,C,H,W]`.

## Compiling

```bash
amdva-compile-model model.onnx model.mxr --batch=4 --precision=fp16 \
  --model-info=model-info.json [--input-shape=NAME:D1,D2,...]
```

The compiler writes `model.mxr.manifest.json` (schema 2) containing the SHA256,
GPU architecture, batch size and `offload_copy=false`. At load time the runtime
refuses a model whose manifest is missing, whose hash differs, or that was built
for a different GPU architecture. Rebuild when you change GPU or batch size.
`--precision=fp16` (default) changes internal computation only; external tensors
keep their original types.

The elements require one float32 NCHW image input. Multi-input models and other
tensor types (float16, int32, int64, uint8) are available through the C++
`ModelExecutor` API.

## Mapping from other formats

| Other format | Field here |
| --- | --- |
| DL Streamer `model-proc` `input_preproc.resize`, `color_space` | `input.letterbox`, `input.channel_order` |
| DL Streamer `model-proc` `output_postproc.converter` | `adapter` / `task_type` |
| DL Streamer `labels` | `labels` |
| DeepStream `net-scale-factor` | `input.scale` |
| DeepStream `offsets` | `input.mean` (in scaled units: `offset * scale`) |
| DeepStream `model-color-format` (0 RGB, 1 BGR) | `input.channel_order` |
| DeepStream `maintain-aspect-ratio` | `input.letterbox` |
| DeepStream `num-detected-classes` | `output.num_classes` |
| DeepStream `labelfile-path` | `labels` |
| DeepStream `batch-size` | element `batch-size` and `amdva-compile-model --batch` |
