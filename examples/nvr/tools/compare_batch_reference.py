#!/usr/bin/env python3
"""Offline CPU reference for distinct images in full and partial GPU batches."""
import argparse
import json
from pathlib import Path
import numpy as np
import onnxruntime as ort


def iou(a, b):
    right = np.minimum(a[:2] + a[2:4], b[:2] + b[2:4])
    left = np.maximum(a[:2], b[:2])
    intersection = np.maximum(0, right - left).prod()
    return intersection / max(1e-9, a[2:4].prod() + b[2:4].prod() - intersection)


def decode(raw):
    boxes = []
    for row in raw.T:
        category = int(row[4:].argmax())
        score = float(row[4 + category])
        if score >= 0.25:
            boxes.append(np.array([row[0] - row[2] / 2, row[1] - row[3] / 2,
                                   row[2], row[3], score, category]))
    boxes.sort(key=lambda x: -x[4])
    result = []
    while boxes and len(result) < 300:
        best = boxes.pop(0)
        result.append(best)
        boxes = [b for b in boxes if b[5] != best[5] or iou(best, b) <= 0.45]
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('onnx')
    parser.add_argument('input')
    parser.add_argument('detections')
    args = parser.parse_args()
    tensor = np.fromfile(args.input, dtype=np.float32).reshape(4, 3, 640, 640)
    session = ort.InferenceSession(args.onnx, providers=['CPUExecutionProvider'])
    cases = json.loads(Path(args.detections).read_text())
    minimum, delta, matched = 1.0, 0.0, 0
    for case in cases:
        valid = case['valid']
        raw = session.run(None, {session.get_inputs()[0].name: tensor[:valid]})[0]
        assert len(case['images']) == valid
        for slot, actual in enumerate(case['images']):
            expected = decode(raw[slot])
            assert len(actual) == len(expected), (valid, slot, len(actual), len(expected))
            unused = list(range(len(expected)))
            for box in actual:
                box = np.array(box)
                candidates = [j for j in unused if expected[j][5] == box[5]]
                assert candidates, (valid, slot, box)
                index = max(candidates, key=lambda j: iou(box, expected[j]))
                overlap = iou(box, expected[index])
                confidence = abs(box[4] - expected[index][4])
                assert overlap >= 0.95 and confidence <= 0.03, (valid, slot, overlap, confidence)
                unused.remove(index)
                minimum, delta, matched = min(minimum, overlap), max(delta, confidence), matched + 1
    print(json.dumps({'passed': True, 'valid_batches': [c['valid'] for c in cases],
                      'matched': matched, 'minimum_iou': minimum, 'maximum_score_delta': delta}))


if __name__ == '__main__':
    main()
