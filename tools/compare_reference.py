#!/usr/bin/env python3
"""Compare FP16 GPU detections against FP32 CPU ONNX on the same input tensor."""
import argparse
import json
from pathlib import Path
import numpy as np
import onnxruntime as ort


def iou(a, b):
    low = np.maximum(a[:2], b[:2])
    high = np.minimum(a[2:], b[2:])
    intersection = np.prod(np.maximum(0, high - low))
    union = np.prod(a[2:] - a[:2]) + np.prod(b[2:] - b[:2]) - intersection
    return float(intersection / union) if union > 0 else 0.0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("onnx")
    p.add_argument("tensor")
    p.add_argument("geometry")
    p.add_argument("events")
    p.add_argument("labels")
    args = p.parse_args()
    geometry = json.loads(Path(args.geometry).read_text())
    labels = Path(args.labels).read_text().splitlines()
    tensor = np.fromfile(args.tensor, dtype=np.float32).reshape(1, 3, 640, 640)
    session = ort.InferenceSession(args.onnx, providers=["CPUExecutionProvider"])
    raw = session.run(None, {session.get_inputs()[0].name: tensor})[0][0].T
    classes = raw[:, 4:].argmax(axis=1)
    scores = raw[:, 4:].max(axis=1)
    boxes = np.concatenate([raw[:, :2] - raw[:, 2:4] / 2, raw[:, :2] + raw[:, 2:4] / 2], axis=1)
    candidates = np.flatnonzero(scores >= .25).tolist()
    candidates.sort(key=lambda index: float(scores[index]), reverse=True)
    selected = []
    while candidates and len(selected) < 300:
        best = candidates.pop(0)
        selected.append(best)
        candidates = [index for index in candidates if classes[index] != classes[best]
                      or iou(boxes[best], boxes[index]) <= .45]
    events = [json.loads(line) for line in Path(args.events).read_text().splitlines() if line.strip()]
    frames = [e for e in events if e["timestamp"] == geometry["pts"]]
    if len(frames) != 1:
        p.error("expected exactly one event for reference PTS; use a fresh events file")
    frame = frames[0]
    gpu = frame.get("objects", [])
    matches = []
    used = set()
    for index in selected:
        b = boxes[index].copy()
        b[[0, 2]] = (b[[0, 2]] - geometry["pad_x"]) * geometry["width"] / geometry["scaled_width"]
        b[[1, 3]] = (b[[1, 3]] - geometry["pad_y"]) * geometry["height"] / geometry["scaled_height"]
        b[:2] = np.floor(b[:2]); b[2:] = np.ceil(b[2:])
        b[[0, 2]] = np.clip(b[[0, 2]], 0, geometry["width"])
        b[[1, 3]] = np.clip(b[[1, 3]], 0, geometry["height"])
        if np.any(b[2:] <= b[:2]):
            continue
        choices = [(iou(b, np.array([o["x"], o["y"], o["x"] + o["w"], o["y"] + o["h"]])), j)
                   for j, o in enumerate(gpu) if j not in used and o["detection"]["label"] == labels[classes[index]]]
        overlap, match = max(choices, default=(0, -1))
        difference = abs(float(scores[index]) - gpu[match]["detection"]["confidence"]) if match >= 0 else 1
        ok = overlap >= .90 and difference <= .05
        if ok:
            used.add(match)
        matches.append({"label": labels[classes[index]], "iou": overlap,
                        "confidence_delta": difference, "passed": ok})
    passed = bool(matches and all(m["passed"] for m in matches) and len(used) == len(gpu))
    print(json.dumps({"passed": passed, "cpu_count": len(matches), "gpu_count": len(gpu),
                      "tolerances": {"min_iou": .90, "max_confidence_delta": .05}, "matches": matches}, indent=2))
    return 0 if passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
