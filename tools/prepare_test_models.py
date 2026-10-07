#!/usr/bin/env python3
"""Generate small deterministic ONNX fixtures and compile them on the local GPU.
Run with .deps/model-export/bin/python after sourcing scripts/env.sh.
"""
import argparse
import json
from pathlib import Path
import subprocess
import onnx
from onnx import TensorProto as T, helper as H

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--compiler', type=Path, default=Path('build/src/gst/amdva-compile-model'))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
info = args.output / 'model.json'
info.write_text(json.dumps({'task_type': 'classification', 'input': {'shape': [1, 3, 8, 8]},
                            'output': {'num_classes': 3, 'top_k': 3}}, indent=2) + '\n')
for multi in (False, True):
    inputs = [H.make_tensor_value_info('images', T.FLOAT, ['batch', 3, 8, 8])]
    nodes = [H.make_node('ReduceMean', ['images'], ['mean'], axes=[2, 3], keepdims=0)]
    outputs = [H.make_tensor_value_info('mean', T.FLOAT, ['batch', 3])]
    if multi:
        inputs.append(H.make_tensor_value_info('bias', T.FLOAT, ['batch', 3]))
        nodes.append(H.make_node('Add', ['mean', 'bias'], ['sum']))
        outputs.insert(0, H.make_tensor_value_info('sum', T.FLOAT, ['batch', 3]))
    model = H.make_model(H.make_graph(nodes, 'gpu-contract', inputs, outputs),
                         opset_imports=[H.make_opsetid('', 13)], ir_version=8)
    onnx.checker.check_model(model)
    path = args.output / ('multi.onnx' if multi else 'batch.onnx')
    onnx.save(model, path)
    for batch in ([1] if multi else [1, 2, 4, 8, 16]):
        output = args.output / ('multi.mxr' if multi else f'batch{batch}.mxr')
        command = [str(args.compiler), str(path), str(output), f'--batch={batch}',
                   f'--model-info={info}', '--precision=fp32']
        if multi:
            command.append('--input-shape=bias:1,3')
        with output.with_suffix('.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=log, check=True)
print(args.output)
