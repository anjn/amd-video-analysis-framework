#!/usr/bin/env python3
"""Benchmark the production native libeg2 ABI (no Python inference runtime).
Inputs are deterministic synthetic RGB. This measures latency, not search quality.
Profiles must already exist. Each result is committed atomically for resumability.
"""

import argparse, ctypes as C, json, statistics, time, resource
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--library", required=True)
p.add_argument("--bundle", required=True)
p.add_argument("--output", type=Path, default=Path("artifacts/ai-benchmark/embedding"))
p.add_argument("--seconds", type=float, default=30)
p.add_argument("--repeats", type=int, default=3)
p.add_argument("--iterations", type=int, default=200)
p.add_argument("--warmup", type=int, default=20)
p.add_argument("--sizes", nargs="+", help="Restrict inputs, e.g. 108x192")
p.add_argument("--batches", type=int, nargs="+", default=[1, 2, 4, 8])
a = p.parse_args()
if (
    a.seconds <= 0
    or a.repeats < 1
    or a.iterations < 1
    or a.warmup < 0
    or any(b < 1 or b > 8 for b in a.batches)
):
    p.error("Positive duration/repeats/iterations and batch 1..8 required")
failed = False
a.output.mkdir(parents=True, exist_ok=True)
lib = C.CDLL(a.library)
context = C.c_void_p()
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
if lib.eg2_create(a.bundle.encode(), 0, C.byref(context), error, len(error)):
    raise RuntimeError(error.value.decode())
try:
    for w, h in [(384, 384), (256, 256), (144, 256), (108, 192), (72, 128)]:
        if a.sizes and f"{w}x{h}" not in a.sizes:
            continue
        for batch in a.batches:
            path = a.output / f"{w}x{h}-b{batch}.json"
            if path.exists():
                cached = json.loads(path.read_text())
                if not cached.get("error"):
                    continue
            rgb = (C.c_uint8 * (batch * w * h * 3))(
                *((i * 17) % 256 for i in range(batch * w * h * 3))
            )
            vectors = (C.c_float * (batch * 768))()

            def infer():
                if lib.eg2_encode_images(
                    context,
                    rgb,
                    batch,
                    h,
                    w,
                    70,
                    vectors,
                    len(vectors),
                    error,
                    len(error),
                ):
                    raise RuntimeError(error.value.decode())

            result = dict(
                width=w,
                height=h,
                batch=batch,
                bundle=a.bundle,
                timing="completed native wall time, includes preprocessing and output transfer",
                runs=[],
            )
            try:
                for _ in range(a.warmup):
                    infer()
                for repeat in range(a.repeats):
                    values = []
                    start = time.monotonic()
                    while (
                        len(values) < a.iterations
                        or time.monotonic() - start < a.seconds
                    ):
                        t = time.perf_counter()
                        infer()
                        values.append((time.perf_counter() - t) * 1000)
                    values.sort()
                    mean = statistics.mean(values)
                    result["runs"].append(
                        dict(
                            samples=len(values),
                            mean_ms=mean,
                            p50_ms=values[len(values) // 2],
                            p95_ms=values[int(len(values) * 0.95)],
                            max_ms=max(values),
                            per_image_ms=mean / batch,
                        )
                    )
            except Exception as e:
                result["error"] = str(e)
                failed = True
            result["max_rss_mib"] = (
                resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
            )
            tmp = path.with_suffix(".tmp")
            tmp.write_text(json.dumps(result, indent=2) + "\n")
            tmp.replace(path)
            print(path, result.get("error", result["runs"]), flush=True)
finally:
    lib.eg2_destroy(context)

if failed:
    raise SystemExit(1)
