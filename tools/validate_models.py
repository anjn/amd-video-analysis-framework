#!/usr/bin/env python3
"""Compare offline GPU outputs with ONNX Runtime CPU on the same VPE input.
Invoke amdva-validate-model for each model before this script. Tensor/image
readback is confined to that offline tool; runtime plugins never use these files.
"""
import argparse
import json
from pathlib import Path
import numpy as np
import onnxruntime as ort


def iou(a, b):
    left = np.maximum(a[:2], b[:2])
    right = np.minimum(a[2:], b[2:])
    overlap = np.maximum(right - left, 0).prod()
    return float(overlap / max(1e-8, np.maximum(a[2:] - a[:2], 0).prod() +
                              np.maximum(b[2:] - b[:2], 0).prod() - overlap))


def compare(name, directory, detection, video_size):
    prefix = directory / f'{name}-reference'
    onnx = directory / f'{name}.onnx' if name != 'yolov8n' else detection / 'yolov8n.onnx'
    info_path = directory / f'{name}.json' if name != 'yolov8n' else detection / 'model-info.json'
    info = json.loads(info_path.read_text())
    shape = info['input']['shape']
    image = np.fromfile(str(prefix) + '.input.f32', np.float32).reshape(shape)
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(onnx), options, providers=['CPUExecutionProvider'])
    cpu = session.run(None, {session.get_inputs()[0].name: image})
    shapes = json.loads(Path(str(prefix) + '.shapes.json').read_text())
    output = json.loads(Path(str(prefix) + '.json').read_text())
    report = {'outputs': []}
    for index, reference in enumerate(cpu):
        gpu = np.fromfile(str(prefix) + f'.output{index}.f32', np.float32).reshape(shapes[index])
        assert gpu.shape == reference.shape and np.isfinite(gpu).all(), 'Invalid GPU tensor'
        relative_error = float(np.linalg.norm(gpu - reference) / max(np.linalg.norm(reference), 1e-8))
        assert relative_error < .01, f'{name}: relative L2 error {relative_error} exceeds 1%'
        report['outputs'].append({'relative_l2_error': relative_error,
                                  'maximum_absolute_error': float(np.abs(gpu-reference).max())})
    task = info.get('task_type', 'detection')
    labels = (info_path.parent / info['labels']).read_text().splitlines()
    if task == 'classification':
        expected = np.argsort(-cpu[0][0])[:info['output'].get('top_k', 5)]
        actual = [labels.index(value['label']) for value in output['classifications']]
        assert actual == expected.tolist(), f'{name}: top-k rankings differ'
        report['top_k_match'] = True
    elif task == 'semantic-segmentation':
        actual = np.fromfile(str(prefix) + '.semantic-mask.i32', np.int32).reshape(cpu[0].shape[2:])
        reference = cpu[0][0].argmax(0)
        agreement = float((actual == reference).mean())
        assert agreement > .99, f'{name}: semantic mask agreement {agreement}'
        report['semantic_mask_agreement'] = agreement
    else:
        classes = info['output']['num_classes']
        raw = cpu[0][0]
        scores = raw[4:4+classes].max(0)
        class_ids = raw[4:4+classes].argmax(0)
        boxes = np.stack((raw[0]-raw[2]/2, raw[1]-raw[3]/2,
                          raw[0]+raw[2]/2, raw[1]+raw[3]/2), axis=1)
        candidates = np.flatnonzero(scores >= .2)
        selected = []
        for index in candidates[np.argsort(-scores[candidates])]:
            if not any(class_ids[index] == class_ids[previous] and
                       iou(boxes[index], boxes[previous]) > .45 for previous in selected):
                selected.append(index)
        width, height = video_size
        scale = min(shape[3]/width, shape[2]/height)
        scaled = np.floor(np.array([width, height])*scale+.5)
        padding = (np.array([shape[3], shape[2]])-scaled)//2
        scale_back = np.array([width, height])/scaled
        source_boxes = (boxes.reshape(-1, 2, 2)-padding)*scale_back
        source_boxes = np.clip(source_boxes, [0, 0], [width, height]).reshape(-1, 4)
        matched = []
        mask_agreements = []
        if task == 'instance-segmentation':
            _, channels, mh, mw = cpu[1].shape
            gpu_masks = np.fromfile(str(prefix) + '.instance-masks.f32', np.float32).reshape(-1, mh, mw)
        for plane, obj in enumerate(output['objects']):
            box = np.array([obj['x'], obj['y'], obj['x']+obj['width'], obj['y']+obj['height']])
            available = [index for index in selected if labels[class_ids[index]] == obj['label']]
            index = max(available, key=lambda index: iou(box, source_boxes[index]))
            assert iou(box, source_boxes[index]) > .9 and abs(obj['confidence']-scores[index]) < .04, f'{name}: object differs'
            matched.append(index)
            if task == 'pose':
                points = raw[4+classes:, index].reshape(-1, 3)
                actual = np.array([[p['x'], p['y']] for p in obj['keypoints']])
                reference = np.clip((points[:, :2]-padding)*scale_back, [0, 0], [width, height])
                confident = points[:, 2] >= .5
                assert actual.shape == reference.shape and np.max(np.abs(actual[confident]-reference[confident]), initial=0) < 5, 'Pose coordinates differ'
            if task == 'instance-segmentation':
                logits = raw[4+classes:4+classes+channels, index] @ cpu[1][0].reshape(channels, -1)
                mask = logits.reshape(mh, mw) > 0
                x = (np.arange(mw)+.5)*shape[3]/mw
                y = (np.arange(mh)+.5)*shape[2]/mh
                crop = (x[None, :] >= boxes[index, 0]) & (x[None, :] < boxes[index, 2]) & (y[:, None] >= boxes[index, 1]) & (y[:, None] < boxes[index, 3])
                mask &= crop
                actual = gpu_masks[plane] >= .5
                agreement = float((actual & mask).sum()/max(1, (actual | mask).sum()))
                assert agreement > .95, f'{name}: instance mask IoU {agreement}'
                mask_agreements.append(agreement)
        assert all(index in matched for index in selected if scores[index] >= .3), f'{name}: confident reference object missing'
        report['objects_matched'] = len(matched)
        if mask_agreements:
            report['minimum_instance_mask_iou'] = min(mask_agreements)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('models', type=Path)
    parser.add_argument('detection', type=Path)
    parser.add_argument('--video-size', nargs=2, type=int, default=[1920, 1080])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = {name: compare(name, args.models, args.detection, args.video_size)
              for name in ('resnet18', 'yolov8n-seg', 'yolov8n-pose', 'segformer-b0', 'yolov8n')}
    args.output.write_text(json.dumps({'passed': True, 'models': report}, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
