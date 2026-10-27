#!/usr/bin/env python3
"""Export the same YOLOv8 weights with dynamic batch, then prepare GPU models.
Run in the separate environment created by scripts/setup_model_export.py.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import json
import hashlib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('weights', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--batch', type=int, choices=(1, 2, 4, 8, 16), default=4)
    args = parser.parse_args()
    if not args.weights.is_file():
        parser.error('weights file does not exist')
    os.environ['YOLO_AUTOINSTALL'] = 'false'
    from ultralytics import YOLO
    args.output.mkdir(parents=True, exist_ok=True)
    local = args.output / 'yolov8n.pt'
    if local.resolve() != args.weights.resolve():
        shutil.copyfile(args.weights, local)
    model = YOLO(str(local))
    if model.task != 'detect' or len(model.names) != 80:
        parser.error('YOLOv8 COCO detection weights required')
    # A CPU export sets CUDA_VISIBLE_DEVICES=-1, which would also hide the GPU from HIP.
    env = dict(os.environ)
    onnx = model.export(format='onnx', imgsz=640, dynamic=True, batch=args.batch,
                        opset=13, simplify=True, device='cpu')
    for batch in sorted({1, args.batch}):
        target = args.output / f'yolov8n-b{batch}.mxr'
        subprocess.run(['amdva-compile-model', str(onnx), str(target), f'--batch={batch}'],
                       check=True, env=env)
    (args.output / 'coco80.txt').write_text(''.join(model.names[i] + '\n' for i in range(80)))
    root = Path(__file__).resolve().parents[3]
    shutil.copyfile(root / 'examples/yolov8n.json', args.output / 'model-info.json')
    (args.output / 'export-info.json').write_text(json.dumps({
        'weights_sha256': hashlib.sha256(local.read_bytes()).hexdigest(),
        'onnx_sha256': hashlib.sha256(Path(onnx).read_bytes()).hexdigest(),
        'batch': args.batch, 'opset': 13, 'input': 'images',
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
