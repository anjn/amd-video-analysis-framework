#!/usr/bin/env python3
"""Compare live background detections to FP32 ONNX on an offline keyframe tensor."""
import argparse
import json
from pathlib import Path
import numpy as np
import onnxruntime as ort
from compare_batch_reference import decode, iou


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('onnx')
    parser.add_argument('tensor')
    parser.add_argument('telemetry')
    parser.add_argument('--channels', nargs='+', required=True)
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    parser.add_argument('--alternate-tensors', nargs='*', default=[], help='Other encoded keyframe tensors from the same looping fixture')
    parser.add_argument('--allow-fp16-nms-ties', action='store_true', help='Allow a CPU-suppressed candidate within 0.005 of the winning score; retain IoU 0.95 and score delta 0.03 checks')
    args = parser.parse_args()
    session = ort.InferenceSession(args.onnx, providers=['CPUExecutionProvider'])
    references = []
    candidates_by_reference = []
    for path in [args.tensor] + args.alternate_tensors:
        tensor = np.fromfile(path, dtype=np.float32).reshape(1, 3, 640, 640)
        raw = session.run(None, {session.get_inputs()[0].name: tensor})[0][0]
        references.append(decode(raw))
        candidates = []
        for row in raw.T:
            category = int(row[4:].argmax())
            score = float(row[4 + category])
            if score >= 0.25:
                candidates.append(np.array([row[0] - row[2] / 2, row[1] - row[3] / 2, row[2], row[3], score, category]))
        candidates_by_reference.append(candidates)
    scale = min(640 / args.width, 640 / args.height)
    sw, sh = round(args.width * scale), round(args.height * scale)
    px, py = (640 - sw) // 2, (640 - sh) // 2
    for expected in references + candidates_by_reference:
        for b in expected:
            x, y = np.clip((b[:2] - [px, py]) * [args.width / sw, args.height / sh], 0, [args.width, args.height])
            right, bottom = np.clip((b[:2] + b[2:4] - [px, py]) * [args.width / sw, args.height / sh], 0, [args.width, args.height])
            b[:4] = [x, y, right - x, bottom - y]
    rows = [json.loads(line) for line in Path(args.telemetry).read_text().splitlines()]
    matched, minimum, delta, nms_ties, canonical_minimum = 0, 1.0, 0.0, 0, 1.0
    for channel in args.channels:
        state = next(c for r in rows for c in r['channels'] if c['id'] == channel and c['background_ai'] > 0)
        actual = state['background_detections']
        matches = []
        for ref_index, expected in enumerate(references):
            if len(actual) != len(expected):
                continue
            unused = list(range(len(expected)))
            overlaps, confidences, canonical_overlaps, ties = [], [], [], 0
            for d in actual:
                b = np.array([d[k] for k in ('x', 'y', 'w', 'h', 'score', 'class')])
                candidates = [j for j in unused if expected[j][5] == b[5]]
                if not candidates:
                    break
                j = max(candidates, key=lambda j: iou(b, expected[j]))
                chosen = expected[j]
                canonical_overlap = iou(b, chosen)
                canonical_overlaps.append(canonical_overlap)
                if canonical_overlap < 0.95 and args.allow_fp16_nms_ties:
                    alternatives = [a for a in candidates_by_reference[ref_index]
                                    if a[5] == chosen[5] and abs(a[4] - chosen[4]) <= 0.005
                                    and iou(a, chosen) > 0.45]
                    if alternatives:
                        alternative = max(alternatives, key=lambda a: iou(b, a))
                        if iou(b, alternative) > canonical_overlap:
                            chosen = alternative
                            ties += 1
                overlaps.append(iou(b, chosen))
                confidences.append(abs(b[4] - chosen[4]))
                unused.remove(j)
            if len(overlaps) == len(actual):
                matches.append((min(overlaps, default=1.0), max(confidences, default=0.0), ties, min(canonical_overlaps, default=1.0)))
        assert matches, (channel, 'No matching reference classes/count')
        overlap, confidence, ties, canonical_overlap = max(matches, key=lambda v: v[0])
        assert overlap >= 0.95 and confidence <= 0.03, (channel, overlap, confidence)
        matched += len(actual)
        nms_ties += ties
        canonical_minimum = min(canonical_minimum, canonical_overlap)
        minimum, delta = min(minimum, overlap), max(delta, confidence)
    print(json.dumps({'passed': True, 'channels': args.channels, 'reference_keyframes': len(references), 'fp16_nms_ties': nms_ties, 'minimum_canonical_nms_iou': canonical_minimum, 'matched': matched,
                      'minimum_iou': minimum, 'maximum_score_delta': delta}))


if __name__ == '__main__':
    main()
