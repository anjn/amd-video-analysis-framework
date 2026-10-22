#!/usr/bin/env python3
"""Export, compile and benchmark candidate models sequentially on one GPU.
Uses the existing model-export environment. Outputs and failures are resumable.
Inference timings exclude input preparation and do not measure task accuracy.
"""

import argparse, hashlib, importlib.util, json, os, subprocess, urllib.request
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--output", type=Path, default=Path("artifacts/ai-benchmark"))
p.add_argument("--seconds", type=float, default=30)
p.add_argument("--sizes", nargs="+", help="Restrict candidate sizes, e.g. 640x384")
p.add_argument("--batches", nargs="+", type=int, choices=[1, 2, 4, 8, 16])
p.add_argument("--repeats", type=int, default=3)
p.add_argument(
    "--remeasure",
    action="store_true",
    help="Repeat measurements using cached compiled models",
)
p.add_argument(
    "--screen",
    action="store_true",
    help="Screen batch 1/4 first; full run includes 8/16",
)
p.add_argument(
    "--models",
    nargs="+",
    choices=[
        "yolov8n",
        "yolo11n",
        "yolox_nano",
        "osnet_x1_0",
        "osnet_x0_5",
        "osnet_x0_25",
    ],
    default=[
        "yolov8n",
        "yolo11n",
        "yolox_nano",
        "osnet_x1_0",
        "osnet_x0_5",
        "osnet_x0_25",
    ],
)
a = p.parse_args()
if a.seconds <= 0 or a.repeats < 1:
    p.error("Positive seconds and repeats required")
failed = False
root = Path(__file__).resolve().parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
import torch

torch.set_num_threads(2)
os.environ["YOLO_AUTOINSTALL"] = "false"
batches = a.batches or ([1, 4] if a.screen else [1, 4, 8, 16])
manifest_path = out / "matrix.json"
try:
    manifest = json.loads(manifest_path.read_text())
except (OSError, ValueError):
    manifest = []


def save_row(row):
    key = lambda v: (v.get("name"), v.get("width"), v.get("height"), v.get("batch"))
    manifest[:] = [v for v in manifest if key(v) != key(row)]
    manifest.append(row)
    tmp = out / "matrix.json.tmp"
    tmp.write_text(json.dumps(manifest, indent=2) + "\n")
    tmp.replace(out / "matrix.json")


for name in a.models:
    try:
        if name.startswith("yolo") and name != "yolox_nano":
            from ultralytics import YOLO

            weights = (
                root / "artifacts/nvr-model/yolov8n.pt"
                if name == "yolov8n"
                else out / (name + ".pt")
            )
            if not weights.exists():
                urllib.request.urlretrieve(
                    "https://github.com/ultralytics/assets/releases/download/v8.3.0/"
                    + name
                    + ".pt",
                    weights,
                )
            net = YOLO(str(weights)).model.eval().cpu()
            net.fuse()
            from ultralytics.nn.modules import Detect

            for m in net.modules():
                if isinstance(m, Detect):
                    m.export = True
                    m.format = "onnx"
                    m.dynamic = True
            shapes = [(640, 640), (640, 384), (480, 288), (320, 192)]
        elif name.startswith("osnet"):
            source = root / ".deps/reid/osnet.py"
            spec = importlib.util.spec_from_file_location("osnet", source)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            weights = out / (name + "-msmt17.pth")
            if not weights.exists():
                info = json.load(
                    urllib.request.urlopen(
                        "https://huggingface.co/api/models/kaiyangzhou/osnet"
                    )
                )
                f = next(
                    x["rfilename"]
                    for x in info["siblings"]
                    if x["rfilename"].startswith(name + "_msmt17")
                )
                urllib.request.urlretrieve(
                    "https://huggingface.co/kaiyangzhou/osnet/resolve/"
                    + info["sha"]
                    + "/"
                    + f,
                    weights,
                )
            net = getattr(module, name)(num_classes=1, pretrained=False).eval()
            state = torch.load(weights, map_location="cpu", weights_only=True)
            state = state.get("state_dict", state)
            state = {
                k.removeprefix("module."): v
                for k, v in state.items()
                if not k.removeprefix("module.").startswith("classifier.")
            }
            missing, extra = net.load_state_dict(state, strict=False)
            if set(missing) != {"classifier.weight", "classifier.bias"} or extra:
                raise RuntimeError("Incompatible ReID checkpoint")
            shapes = [(128, 256), (96, 192), (64, 128)]
        else:
            # Official fixed-shape ONNX baseline; its decoder differs from YOLOv8.
            weights = out / "yolox_nano.onnx"
            if not weights.exists():
                urllib.request.urlretrieve(
                    "https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_nano.onnx",
                    weights,
                )
            net = None
            shapes = [(416, 416)]
        for w, h in shapes:
            if a.sizes and f"{w}x{h}" not in a.sizes:
                continue
            for batch in ([1] if net is None else batches):
                stem = f"{name}-{w}x{h}-b{batch}"
                onnx = out / (stem + ".onnx")
                mxr = out / (stem + ".mxr")
                result = out / (stem + ".json")
                log = out / (stem + ".log")
                row = {
                    "name": name,
                    "width": w,
                    "height": h,
                    "batch": batch,
                    "weights_sha256": hashlib.sha256(weights.read_bytes()).hexdigest(),
                    "result": str(result),
                }
                try:
                    if a.remeasure or not result.exists() or not result.stat().st_size:
                        if not onnx.exists() or not onnx.stat().st_size:
                            if net is None:
                                onnx.write_bytes(weights.read_bytes())
                            else:
                                torch.onnx.export(
                                    net,
                                    torch.zeros(batch, 3, h, w),
                                    str(onnx.with_suffix(".tmp.onnx")),
                                    input_names=["images"],
                                    output_names=["output"],
                                    opset_version=13,
                                    dynamo=False,
                                )
                                onnx.with_suffix(".tmp.onnx").replace(onnx)
                        manifest_path = mxr.with_suffix(".mxr.manifest.json")
                        try:
                            cached = json.loads(manifest_path.read_text())
                            valid = (
                                mxr.stat().st_size > 0
                                and cached["batch"] == batch
                                and cached["sha256"]
                                == hashlib.sha256(mxr.read_bytes()).hexdigest()
                            )
                        except (OSError, ValueError, KeyError):
                            valid = False
                        if not valid:
                            import onnx as ox

                            input_name = ox.load(str(onnx)).graph.input[0].name
                            with log.open("w") as f:
                                subprocess.run(
                                    [
                                        str(root / "build/src/gst/amdva-compile-model"),
                                        str(onnx),
                                        str(mxr.with_suffix(".mxr.tmp")),
                                        f"--input-shape={input_name}:{batch},3,{h},{w}",
                                        f"--batch={batch}",
                                        "--precision=fp16",
                                    ],
                                    stdout=f,
                                    stderr=subprocess.STDOUT,
                                    check=True,
                                )
                            mxr.with_suffix(".mxr.tmp").replace(mxr)
                            mxr.with_suffix(".mxr.tmp.manifest.json").replace(
                                manifest_path
                            )
                        with result.with_suffix(".json.tmp").open("w") as f, log.open(
                            "a"
                        ) as err:
                            subprocess.run(
                                [
                                    str(
                                        root / "build/src/streaming/amdva-ai-benchmark"
                                    ),
                                    str(mxr),
                                    str(a.seconds),
                                    str(a.repeats),
                                ],
                                stdout=f,
                                stderr=err,
                                check=True,
                            )
                        json.load(result.with_suffix(".json.tmp").open())
                        result.with_suffix(".json.tmp").replace(result)
                    row["status"] = "measured"
                    json.load(result.open())
                except Exception as e:
                    failed = True
                    row["status"] = "failed"
                    row["error"] = str(e)
                save_row(row)
                print(stem, row["status"], flush=True)
    except Exception as e:
        failed = True
        save_row({"name": name, "status": "failed", "error": str(e)})
        print(name, str(e), flush=True)

if failed:
    raise SystemExit(1)
