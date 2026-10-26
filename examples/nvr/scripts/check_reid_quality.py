#!/usr/bin/env python3
"""Evaluate reviewed appearance retrieval; this is not tracking IDF1."""

import argparse, importlib.util, json, urllib.request, subprocess
from pathlib import Path
import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image

p = argparse.ArgumentParser()
p.add_argument(
    "--output", type=Path, default=Path("artifacts/ai-benchmark/reid-quality.json")
)
p.add_argument("--native", action="store_true")
a = p.parse_args()
torch.set_num_threads(2)
spec = importlib.util.spec_from_file_location("osnet", ".deps/reid/osnet.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
labels = json.load(open("examples/nvr/benchmarks/appearance-review.json"))["labels"]
rows = []
for name in ["osnet_x1_0", "osnet_x0_5", "osnet_x0_25"]:
    weights = Path("artifacts/ai-benchmark") / (name + "-msmt17.pth")
    if not weights.exists():
        info = json.load(
            urllib.request.urlopen(
                "https://huggingface.co/api/models/kaiyangzhou/osnet", timeout=30
            )
        )
        f = next(
            x["rfilename"]
            for x in info["siblings"]
            if x["rfilename"].startswith(name + "_msmt17")
        )
        urllib.request.urlretrieve(
            "https://huggingface.co/kaiyangzhou/osnet/resolve/" + info["sha"] + "/" + f,
            weights,
        )
    net = getattr(module, name)(num_classes=1, pretrained=False).eval()
    state = torch.load(weights, map_location="cpu", weights_only=True)
    state = state.get("state_dict", state)
    net.load_state_dict(
        {
            k.removeprefix("module."): v
            for k, v in state.items()
            if not k.removeprefix("module.").startswith("classifier.")
        },
        strict=False,
    )
    for w, h in [(128, 256), (96, 192), (64, 128)]:
        vectors, inputs = [], []
        with torch.inference_mode():
            for filename in labels:
                im = np.array(
                    Image.open(Path("artifacts/ai-benchmark/quality") / filename)
                )
                x = torch.tensor(im).permute(2, 0, 1).unsqueeze(0).float() / 255
                x = F.interpolate(x, (h, w), mode="bilinear", align_corners=False)
                x = (
                    x - torch.tensor([0.485, 0.456, 0.406])[None, :, None, None]
                ) / torch.tensor([0.229, 0.224, 0.225])[None, :, None, None]
                vectors.append(F.normalize(net(x), dim=1)[0])
                inputs.append(x)
        v = torch.stack(vectors)
        parity = None
        if a.native:
            stem = Path("artifacts/ai-benchmark") / f"{name}-{w}x{h}-b4"
            torch.cat(inputs).numpy().tofile(str(stem) + ".input.f32")
            subprocess.run(
                [
                    "build/examples/nvr/amdva-ai-infer",
                    str(stem) + ".mxr",
                    str(stem) + ".input.f32",
                    str(stem) + ".output.f32",
                ],
                check=True,
            )
            native = F.normalize(
                torch.from_numpy(
                    np.fromfile(str(stem) + ".output.f32", dtype=np.float32).reshape(
                        -1, 512
                    )[: len(labels)]
                ),
                dim=1,
            )
            parity = float((native * v).sum(1).min())
            if parity < 0.995:
                raise RuntimeError(f"CPU/GPU feature mismatch: {parity}")
            v = native
        sim = v @ v.T
        sim.fill_diagonal_(-1)
        ids = list(labels.values())
        nearest = sim.argmax(1).tolist()
        scores = []
        for i in range(len(ids)):
            same = [
                float(sim[i, j]) for j in range(len(ids)) if i != j and ids[i] == ids[j]
            ]
            if same:
                scores.append(max(same))
        row = dict(
            model=name,
            width=w,
            height=h,
            samples=len(ids),
            native_min_cosine=parity,
            rank1=sum(ids[i] == ids[j] for i, j in enumerate(nearest)) / len(ids),
            best_same_mean=sum(scores) / len(scores),
            same_above_085=sum(s >= 0.85 for s in scores) / len(scores),
        )
        rows.append(row)
        print(row, flush=True)
    a.output.write_text(
        json.dumps(
            {
                "scope": (
                    "Native FP16 appearance retrieval with CPU parity, not IDF1"
                    if a.native
                    else "CPU reference appearance retrieval, not IDF1"
                ),
                "results": rows,
            },
            indent=2,
        )
        + "\n"
    )
