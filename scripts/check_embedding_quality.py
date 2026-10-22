#!/usr/bin/env python3
"""Native libeg2 retrieval on visually reviewed clothing queries, not a general benchmark."""

import argparse, ctypes as C, json
from pathlib import Path
import numpy as np
from PIL import Image

p = argparse.ArgumentParser()
p.add_argument("--library", required=True)
p.add_argument("--bundle", required=True)
p.add_argument(
    "--output", type=Path, default=Path("artifacts/ai-benchmark/embedding-quality.json")
)
p.add_argument("--batch", type=int, default=1)
a = p.parse_args()
lib = C.CDLL(a.library)
ctx = C.c_void_p()
error = C.create_string_buffer(4096)
lib.eg2_create.argtypes = [
    C.c_char_p,
    C.c_int,
    C.POINTER(C.c_void_p),
    C.c_char_p,
    C.c_size_t,
]
lib.eg2_destroy.argtypes = [C.c_void_p]
lib.eg2_encode_images.argtypes = [
    C.c_void_p,
    C.POINTER(C.c_uint8),
    C.c_size_t,
    C.c_size_t,
    C.c_size_t,
    C.c_int,
    C.POINTER(C.c_float),
    C.c_size_t,
    C.c_char_p,
    C.c_size_t,
]
lib.eg2_encode_texts.argtypes = [
    C.c_void_p,
    C.POINTER(C.c_char_p),
    C.c_size_t,
    C.c_char_p,
    C.POINTER(C.c_float),
    C.c_size_t,
    C.c_char_p,
    C.c_size_t,
]


def check(rc):
    if rc:
        raise RuntimeError(error.value.decode())


check(lib.eg2_create(a.bundle.encode(), 0, C.byref(ctx), error, len(error)))
labels = json.load(open("benchmarks/appearance-review.json"))["labels"]
queries = {
    "a person wearing a purple skirt": "D",
    "a person dressed in black with long dark hair": "B",
    "a man wearing a brown checked shirt and blue trousers": "C",
    "a person wearing a green striped sweater and beige trousers": "M",
    "a person wearing a white jacket and beige trousers": "E",
    "a woman with long brown hair wearing a beige skirt": "P",
    "a person wearing a gray shirt and pink trousers": "G",
    "a person wearing a hat and a backpack": "H",
    "a person wearing a brown jacket and carrying a blue backpack": "A",
    "a person wearing a black striped shirt and blue trousers": "L",
    "a person wearing a white blouse and turquoise trousers": "J",
}
rows = []
try:
    texts = []
    for text in queries:
        strings = (C.c_char_p * 1)(text.encode())
        v = (C.c_float * 768)()
        check(
            lib.eg2_encode_texts(
                ctx,
                strings,
                1,
                b"task: search result | query: ",
                v,
                768,
                error,
                len(error),
            )
        )
        texts.append(np.array(v))
    for w, h in [(384, 384), (256, 256), (144, 256), (108, 192), (72, 128)]:
        vectors, prepared = [], []
        for filename in labels:
            im = Image.open(Path("artifacts/ai-benchmark/quality") / filename).convert(
                "RGB"
            )
            im = im.resize((im.width * 2, im.height * 2), Image.Resampling.BILINEAR)
            scale = min(1, w / im.width, h / im.height)
            im = im.resize(
                (max(1, int(im.width * scale)), max(1, int(im.height * scale))),
                Image.Resampling.BILINEAR,
            )
            canvas = Image.new("RGB", (w, h))
            canvas.paste(im, ((w - im.width) // 2, (h - im.height) // 2))
            prepared.append(canvas.tobytes())
        for offset in range(0, len(prepared), a.batch):
            chunk = prepared[offset : offset + a.batch]
            count = len(chunk)
            rgb = (C.c_uint8 * (count * w * h * 3)).from_buffer_copy(b"".join(chunk))
            v = (C.c_float * (count * 768))()
            check(
                lib.eg2_encode_images(
                    ctx, rgb, count, h, w, 70, v, count * 768, error, len(error)
                )
            )
            vectors.extend(np.array(v).reshape(count, 768))
        sims = np.array(texts) @ np.array(vectors).T
        ids = list(labels.values())
        scores = []
        for i, (query, label) in enumerate(queries.items()):
            top = np.argsort(sims[i])[::-1][:5]
            hits = sum(ids[j] == label for j in top)
            scores.append(
                dict(
                    query=query,
                    hits=hits,
                    relevant=ids.count(label),
                    top=[list(labels)[j] for j in top],
                )
            )
        row = dict(
            batch=a.batch,
            width=w,
            height=h,
            hit_at_5=sum(s["hits"] > 0 for s in scores) / len(scores),
            recall_at_5=sum(s["hits"] / s["relevant"] for s in scores) / len(scores),
            queries=scores,
        )
        rows.append(row)
        print(w, h, row["hit_at_5"], row["recall_at_5"], flush=True)
        a.output.write_text(
            json.dumps(
                {
                    "scope": "Native image/text retrieval, 11 manually labelled clothing queries on 52 reviewed crops. Not a comprehensive search-quality guarantee.",
                    "results": rows,
                },
                indent=2,
            )
            + "\n"
        )
finally:
    lib.eg2_destroy(ctx)
