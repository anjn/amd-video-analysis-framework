#!/usr/bin/env python3
"""Create a local libeg2 bundle using symlinks to prepared profiles and text assets."""

import argparse, json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--output", type=Path, required=True)
p.add_argument("bundles", type=Path, nargs="+")
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
merged = None
seen = set()
for root in a.bundles:
    root = root.resolve()
    index = json.loads((root / "index.json").read_text())
    if merged is None:
        merged = {
            k: v for k, v in index.items() if k not in ("profiles", "text_profiles")
        }
        merged.update(profiles=[], text_profiles=[])
    if any(index.get(k) != merged.get(k) for k in ["model", "revision"]):
        raise ValueError("Model revisions differ")
    for key in ["profiles", "text_profiles"]:
        for profile in index.get(key, []):
            identity = json.dumps(profile, sort_keys=True)
            if identity in seen:
                continue
            seen.add(identity)
            merged[key].append(profile)
            dest = a.output / profile["path"]
            if not dest.exists():
                dest.symlink_to((root / profile["path"]).resolve())
    if index.get("tokenizer"):
        path = index["tokenizer"]
        dest = a.output / path
        if not dest.exists():
            dest.symlink_to((root / path).resolve())
        merged["tokenizer"] = path
(a.output / "index.json").write_text(json.dumps(merged, indent=2) + "\n")
