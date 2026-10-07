#!/usr/bin/env python3
"""Export representative static ONNX models with their AMD preprocessing contract.
Run in scripts/setup_model_export.py's separate CPU export environment.
Downloads model weights from their upstream publishers into the given directory.
Model weight licenses remain those of their publishers; see docs/framework.md.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import torch

SEGFORMER_REPO = 'nvidia/segformer-b0-finetuned-ade-512-512'
SEGFORMER_REVISION = '489d5cd81a0b59fab9b7ea758d3548ebe99677da'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--tasks', nargs='+', choices=('classification', 'instance', 'pose', 'semantic'),
                        default=['classification', 'instance', 'pose', 'semantic'])
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.environ['YOLO_AUTOINSTALL'] = 'false'
    manifest = {'torch': torch.__version__, 'models': {}}

    def metadata(name, task, labels, size, normalize=False, input_name='images'):
        (output / f'{name}.txt').write_text('\n'.join(labels) + '\n')
        image = {'name': input_name, 'shape': [1, 3, size, size],
                 'letterbox': task not in ('classification', 'semantic-segmentation')}
        if normalize:
            image.update(mean=[.485, .456, .406], std=[.229, .224, .225])
        (output / f'{name}.json').write_text(json.dumps({
            'task_type': task, 'adapter': 'yolov8' if task in ('pose', 'instance-segmentation') else task,
            'input': image, 'output': {'num_classes': len(labels), 'top_k': 5, 'keypoints': 17},
            'labels': f'{name}.txt'}, indent=2) + '\n')

    def export(model, name, size, input_name='images'):
        torch.onnx.export(model.eval(), torch.zeros(1, 3, size, size), str(output / f'{name}.onnx'),
                          input_names=[input_name], output_names=['logits'], opset_version=17, dynamo=False)

    if 'classification' in args.tasks:
        from torchvision.models import resnet18, ResNet18_Weights
        weights = ResNet18_Weights.IMAGENET1K_V1
        export(resnet18(weights=weights), 'resnet18', 224)
        metadata('resnet18', 'classification', weights.meta['categories'], 224, True)
        manifest['models']['resnet18'] = {'source': weights.url, 'weights': 'IMAGENET1K_V1'}
    for task, name in [('instance', 'yolov8n-seg'), ('pose', 'yolov8n-pose')]:
        if task not in args.tasks:
            continue
        from ultralytics import YOLO
        previous = Path.cwd()
        try:
            os.chdir(output)
            model = YOLO(f'{name}.pt')
            path = Path(model.export(format='onnx', imgsz=640, batch=1, opset=17,
                                     simplify=False, dynamic=False, device='cpu'))
            if path.resolve() != output / f'{name}.onnx':
                shutil.copy2(path, output / f'{name}.onnx')
        finally:
            os.chdir(previous)
        metadata(name, 'instance-segmentation' if task == 'instance' else 'pose',
                 [model.names[i] for i in range(len(model.names))], 640)
        manifest['models'][name] = {'source': f'https://github.com/ultralytics/assets/releases/download/v8.3.0/{name}.pt',
                                    'weights_sha256': hashlib.sha256((output / f'{name}.pt').read_bytes()).hexdigest()}
    if 'semantic' in args.tasks:
        from transformers import SegformerForSemanticSegmentation
        model = SegformerForSemanticSegmentation.from_pretrained(SEGFORMER_REPO, revision=SEGFORMER_REVISION)
        export(model, 'segformer-b0', 512, 'pixel_values')
        metadata('segformer-b0', 'semantic-segmentation', [model.config.id2label[i] for i in range(150)],
                 512, True, 'pixel_values')
        manifest['models']['segformer-b0'] = {'source': f'https://huggingface.co/{SEGFORMER_REPO}', 'revision': SEGFORMER_REVISION}
    for name, model in manifest['models'].items():
        model['onnx_sha256'] = hashlib.sha256((output / f'{name}.onnx').read_bytes()).hexdigest()
    (output / 'export-info.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
