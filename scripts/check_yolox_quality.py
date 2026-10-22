#!/usr/bin/env python3
"""Official YOLOX Nano ONNX contract: BGR 0..255, top-left 114 padding,
stride-grid decoding and objectness × person score. See upstream:
https://github.com/Megvii-BaseDetection/YOLOX/blob/main/demo/ONNXRuntime/onnx_inference.py
"""

import json
from pathlib import Path
import cv2, numpy as np, onnxruntime as ort

options = ort.SessionOptions()
options.intra_op_num_threads = 2
options.inter_op_num_threads = 1
session = ort.InferenceSession(
    "artifacts/ai-benchmark/yolox_nano.onnx",
    options,
    providers=["CPUExecutionProvider"],
)
grid = []
strides = []
for s in [8, 16, 32]:
    y, x = np.mgrid[: 416 // s, : 416 // s]
    grid.append(np.stack([x, y], -1).reshape(-1, 2))
    strides.extend([s] * x.size)
grid = np.concatenate(grid)
strides = np.array(strides)[:, None]


def iou(a, b):
    inter = max(0, min(a[2], b[2]) - max(a[0], b[0])) * max(
        0, min(a[3], b[3]) - max(a[1], b[1])
    )
    return inter / max(
        1, (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    )


counts = {str(t): 0 for t in [0.1, 0.5, 0.6]}
total = 0
for name, truth in json.load(open("benchmarks/person-review.json"))["frames"].items():
    im = cv2.imread("artifacts/ai-benchmark/quality/" + name + ".png")
    r = min(416 / im.shape[0], 416 / im.shape[1])
    canvas = np.full((416, 416, 3), 114, dtype=np.uint8)
    resized = cv2.resize(im, (int(im.shape[1] * r), int(im.shape[0] * r)))
    canvas[: resized.shape[0], : resized.shape[1]] = resized
    output = session.run(
        None,
        {
            session.get_inputs()[0]
            .name: canvas.transpose(2, 0, 1)[None]
            .astype(np.float32)
        },
    )[0][0]
    xy = (output[:, :2] + grid) * strides
    wh = np.exp(output[:, 2:4]) * strides
    boxes = np.concatenate([xy - wh / 2, xy + wh / 2], 1) / r
    scores = output[:, 4] * output[:, 5]
    keep = []
    for j in scores.argsort()[::-1]:
        if scores[j] < 0.1:
            break
        if all(iou(boxes[j], boxes[k]) <= 0.45 for k in keep):
            keep.append(j)
    total += len(truth)
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
                counts[str(t)] += 1
                ug.add(i)
                ub.add(j)
row = dict(
    model="yolox_nano",
    width=416,
    height=416,
    targets=total,
    hits=counts,
    recall={t: c / total for t, c in counts.items()},
)
print(row)
Path("artifacts/ai-benchmark/yolox-quality.json").write_text(
    json.dumps(row, indent=2) + "\n"
)
