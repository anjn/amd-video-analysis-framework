#!/usr/bin/env python3
"""Compare selected native FP16 output with the fused FP32 source model."""

import json, subprocess
from pathlib import Path
import cv2, numpy as np, torch
from ultralytics import YOLO
from ultralytics.nn.modules import Detect

torch.set_num_threads(2)
root = Path("artifacts/ai-benchmark")
model = YOLO(str(root / "yolo11n.pt")).model.eval().cpu()
model.fuse()
for m in model.modules():
    if isinstance(m, Detect):
        m.export = True
        m.format = "onnx"
        m.dynamic = True
images = []
for i in range(1, 17):
    im = cv2.imread(str(root / "quality" / f"M{i:02d}.png"))[:, :, ::-1]
    im = cv2.resize(im, (640, 360))
    canvas = np.full((384, 640, 3), 114, dtype=np.uint8)
    canvas[12:372] = im
    images.append(canvas.transpose(2, 0, 1).astype(np.float32) / 255)
x = np.stack(images)
x.tofile(root / "yolo-native.input.f32")
with torch.inference_mode():
    expected = model(torch.from_numpy(x)).numpy()
subprocess.run(
    [
        "build/src/streaming/amdva-ai-infer",
        str(root / "yolo11n-640x384-b4.mxr"),
        str(root / "yolo-native.input.f32"),
        str(root / "yolo-native.output.f32"),
    ],
    check=True,
)
actual = np.fromfile(root / "yolo-native.output.f32", dtype=np.float32).reshape(
    expected.shape
)
score_error = float(abs(actual[:, 4:] - expected[:, 4:]).max())
mean_box_error = float(abs(actual[:, :4] - expected[:, :4]).mean())
result = dict(
    score_max_error=score_error, box_mean_error=mean_box_error, shape=list(actual.shape)
)
print(result)
if not np.isfinite(actual).all() or score_error > 0.05 or mean_box_error > 1:
    raise RuntimeError("Native YOLO parity failed")
(root / "yolo-native-parity.json").write_text(json.dumps(result, indent=2) + "\n")


def iou(a, b):
    inter = max(0, min(a[2], b[2]) - max(a[0], b[0])) * max(
        0, min(a[3], b[3]) - max(a[1], b[1])
    )
    return inter / max(
        1, (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    )


truths = json.load(open("benchmarks/person-review.json"))["frames"]
hits = {str(t): 0 for t in [0.1, 0.5, 0.6]}
for prediction, truth in zip(actual, truths.values()):
    scores = prediction[4]
    which = prediction[4:].argmax(0)
    xy = prediction[:2].T
    wh = prediction[2:4].T
    boxes = np.concatenate([xy - wh / 2, xy + wh / 2], 1)
    boxes[:, [1, 3]] -= 12
    boxes *= 1.5
    boxes[:, [0, 2]] = np.clip(boxes[:, [0, 2]], 0, 960)
    boxes[:, [1, 3]] = np.clip(boxes[:, [1, 3]], 0, 540)
    keep = []
    for j in scores.argsort()[::-1]:
        if scores[j] < 0.1:
            break
        if which[j] == 0 and all(iou(boxes[j], boxes[k]) <= 0.45 for k in keep):
            keep.append(j)
    for t in [0.1, 0.5, 0.6]:
        pairs = sorted(
            [
                (iou(g, boxes[j]), i, j)
                for i, g in enumerate(truth)
                for j in keep
                if scores[j] >= t and boxes[j, 3] - boxes[j, 1] >= 54
            ],
            reverse=True,
        )
        ug = set()
        ub = set()
        for score, i, j in pairs:
            if score >= 0.5 and i not in ug and j not in ub:
                hits[str(t)] += 1
                ug.add(i)
                ub.add(j)
result["reviewed_hits"] = hits
result["targets"] = sum(map(len, truths.values()))
(root / "yolo-native-parity.json").write_text(json.dumps(result, indent=2) + "\n")
print(result)
