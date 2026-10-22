#!/usr/bin/env python3
"""Make isolated, bounded test copies. Source videos are never modified."""

import argparse, json, subprocess
from pathlib import Path
from PIL import Image

p = argparse.ArgumentParser()
p.add_argument("--videos", type=Path, default=Path.home() / "work/videos")
p.add_argument("--output", type=Path, default=Path("artifacts/ai-benchmark"))
p.add_argument("--seconds", type=int, default=30)
a = p.parse_args()
(a.output / "videos").mkdir(parents=True, exist_ok=True)
(a.output / "quality").mkdir(exist_ok=True)
for n in range(1, 17):
    name = f"M{n:02d}"
    source = next(a.videos.glob(name + "_*.mp4"))
    video = a.output / "videos" / (name + "-1080p.mp4")
    frame = a.output / "quality" / (name + ".png")
    if not video.exists():
        subprocess.run(
            [
                "ffmpeg",
                "-nostdin",
                "-v",
                "error",
                "-threads",
                "2",
                "-i",
                str(source),
                "-t",
                str(a.seconds),
                "-vf",
                "scale=1920:1080",
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-crf",
                "25",
                "-threads",
                "2",
                "-g",
                "30",
                "-an",
                "-y",
                str(video),
            ],
            check=True,
        )
    if not frame.exists():
        subprocess.run(
            [
                "ffmpeg",
                "-nostdin",
                "-v",
                "error",
                "-threads",
                "1",
                "-ss",
                "10",
                "-i",
                str(source),
                "-frames:v",
                "1",
                "-vf",
                "scale=960:540",
                "-threads",
                "1",
                "-y",
                str(frame),
            ],
            check=True,
        )
    print(name, flush=True)
for filename, box in json.load(open("benchmarks/appearance-review.json"))[
    "boxes"
].items():
    name = filename.split("-")[1]
    Image.open(a.output / "quality" / (name + ".png")).crop(
        tuple(map(round, box))
    ).save(a.output / "quality" / filename)
(a.output / "videos.txt").write_text(
    "\n".join(str(f.resolve()) for f in sorted((a.output / "videos").glob("*.mp4")))
    + "\n"
)
