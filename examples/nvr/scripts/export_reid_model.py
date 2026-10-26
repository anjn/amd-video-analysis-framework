#!/usr/bin/env python3
"""Export OSNet x1.0 ReID weights using the upstream osnet.py implementation.
Run in the model-export environment, after sourcing scripts/env.sh.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import torch

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, required=True, help='Upstream torchreid/models/osnet.py')
p.add_argument('--weights', type=Path, required=True, help='OSNet x1.0 MSMT17 ReID checkpoint')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--reference-only', action='store_true', help='Validate export preparation without recompiling')
a = p.parse_args()
spec = importlib.util.spec_from_file_location('osnet', a.source)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
model = m.osnet_x1_0(num_classes=1, pretrained=False).eval()
checkpoint = torch.load(a.weights, map_location='cpu', weights_only=True)
state = checkpoint.get('state_dict', checkpoint)
state = {k.removeprefix('module.'): v for k, v in state.items()}
state = {k: v for k, v in state.items() if not k.startswith('classifier.')}
missing, unexpected = model.load_state_dict(state, strict=False)
if set(missing) != {'classifier.weight', 'classifier.bias'} or unexpected:
    raise RuntimeError(f'Checkpoint incompatible: {missing}, {unexpected}')
a.output.mkdir(parents=True, exist_ok=True)
torch.manual_seed(17)
reference = torch.randint(0, 256, (256, 128, 3), dtype=torch.uint8)
x = reference.permute(2, 0, 1).float().unsqueeze(0) / 255
x = (x - torch.tensor([.485, .456, .406])[None, :, None, None]) / torch.tensor([.229, .224, .225])[None, :, None, None]
with torch.no_grad():
    feature = torch.nn.functional.normalize(model(x), dim=1)
(a.output / 'reference.rgb').write_bytes(reference.numpy().tobytes())
(a.output / 'reference.f32').write_bytes(feature.numpy().tobytes())
if a.reference_only:
    raise SystemExit(0)
onnx = a.output / 'osnet-x1-msmt17.onnx'
torch.onnx.export(model, torch.zeros(1, 3, 256, 128), str(onnx),
                  input_names=['images'], output_names=['features'], opset_version=13,
                  dynamo=False)
subprocess.run(['amdva-compile-model', str(onnx), str(a.output / 'osnet-x1-msmt17.mxr'),
                '--input-shape=images:1,3,256,128', '--precision=fp16'], check=True)
(a.output / 'reid-export.json').write_text(json.dumps({
    'architecture': 'osnet_x1_0', 'dataset': 'MSMT17',
    'weights_sha256': hashlib.sha256(a.weights.read_bytes()).hexdigest(),
    'source_sha256': hashlib.sha256(a.source.read_bytes()).hexdigest(),
    'onnx_sha256': hashlib.sha256(onnx.read_bytes()).hexdigest(),
    'input': [1, 3, 256, 128], 'rgb_mean': [0.485, 0.456, 0.406],
    'rgb_std': [0.229, 0.224, 0.225], 'output': [1, 512],
}, indent=2) + '\n')
