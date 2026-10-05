#!/usr/bin/env python3
"""Create a CPU-only model export environment; never install Torch at runtime."""
import json
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[1]
lock = json.loads((root / 'dependencies.lock.json').read_text())['model_export']
venv = root / '.deps/model-export'
if not (venv / 'bin/python').exists():
    subprocess.run(['uv', 'venv', str(venv)], check=True)
python = venv / 'bin/python'
subprocess.run(['uv', 'pip', 'install', '--python', str(python), '--index-url',
                'https://download.pytorch.org/whl/cpu',
                *(f'{name}=={version}' for name, version in lock['torch'].items())], check=True)
subprocess.run(['uv', 'pip', 'install', '--python', str(python),
                *(f'{name}=={version}' for name, version in lock['python'].items())], check=True)
