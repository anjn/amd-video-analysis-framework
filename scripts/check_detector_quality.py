#!/usr/bin/env python3
"""Recall on the explicitly reviewed visible-person subset; not tracking accuracy."""

import argparse, json
from pathlib import Path
import torch
from ultralytics import YOLO

p = argparse.ArgumentParser()
p.add_argument("--images", type=Path, default=Path("artifacts/ai-benchmark/quality"))
p.add_argument(
    "--output", type=Path, default=Path("artifacts/ai-benchmark/detection-quality.json")
)
a = p.parse_args()
torch.set_num_threads(2)
annotations = json.load(open("benchmarks/person-review.json"))["frames"]
rows = []


def iou(a, b):
    inter = max(0, min(a[2], b[2]) - max(a[0], b[0])) * max(
        0, min(a[3], b[3]) - max(a[1], b[1])
    )
    return inter / max(
        1, (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    )


for name, path in [
    ("yolov8n", "artifacts/nvr-model/yolov8n.pt"),
    ("yolo11n", "artifacts/ai-benchmark/yolo11n.pt"),
]:
    model = YOLO(path)
    for w, h in [(640, 640), (640, 384), (480, 288), (320, 192)]:
        counts = {str(t): 0 for t in [0.1, 0.5, 0.6]}
        total = 0
        for frame, truth in annotations.items():
            result = model.predict(
                str(a.images / (frame + ".png")),
                imgsz=(h, w),
                rect=False,
                device="cpu",
                classes=[0],
                conf=0.1,
                iou=0.45,
                verbose=False,
            )[0]
            boxes = result.boxes.data.tolist()
            total += len(truth)
            for threshold in [0.1, 0.5, 0.6]:
                pairs = sorted(
                    [
                        (iou(g, b), i, j)
                        for i, g in enumerate(truth)
                        for j, b in enumerate(boxes)
                        if b[4] >= threshold and b[3] - b[1] >= 54
                    ],
                    reverse=True,
                )
                usedg = set()
                usedb = set()
                for score, i, j in pairs:
                    if score >= 0.5 and i not in usedg and j not in usedb:
                        counts[str(threshold)] += 1
                        usedg.add(i)
                        usedb.add(j)
        row = dict(
            model=name,
            width=w,
            height=h,
            targets=total,
            hits=counts,
            recall={t: c / total for t, c in counts.items()},
        )
        rows.append(row)
        print(row, flush=True)
    a.output.write_text(
        json.dumps(
            {
                "scope": "Reviewed subset recall, CPU PyTorch reference; not exhaustive accuracy or IDF1",
                "results": rows,
            },
            indent=2,
        )
        + "\n"
    )
