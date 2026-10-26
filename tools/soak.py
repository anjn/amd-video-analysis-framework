#!/usr/bin/env python3
"""Run a paced H.264 loop with copy audit and sample host resources."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import time


def gpu_alloc_kib(pid):
    """Sum allocated VRAM/GTT per DRM client, without duplicate FD aliases."""
    clients = {}
    for path in Path(f"/proc/{pid}/fdinfo").iterdir():
        try:
            fields = dict(line.split(":", 1) for line in path.read_text().splitlines() if ":" in line)
        except (FileNotFoundError, ProcessLookupError):
            continue
        client = fields.get("drm-client-id")
        if not client or client in clients:
            continue
        if not all(key in fields for key in ("drm-total-vram", "drm-total-gtt")):
            continue
        total = 0
        for key in ("drm-total-vram", "drm-total-gtt"):
            amount, unit = fields[key].split()
            total += int(amount) * {"KiB": 1, "MiB": 1024, "GiB": 1048576}[unit]
        clients[client] = total
    return sum(clients.values()) if clients else None


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def process_work(pid):
    """CPU time and DRM engine work, deduplicating aliases of each client."""
    stat = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    cpu = (int(stat[11]) + int(stat[12])) / os.sysconf('SC_CLK_TCK')
    clients = {}
    for path in Path(f'/proc/{pid}/fdinfo').iterdir():
        try:
            fields = dict(line.split(':', 1) for line in path.read_text().splitlines() if ':' in line)
        except (FileNotFoundError, ProcessLookupError):
            continue
        client = fields.get('drm-client-id')
        if client:
            clients[(fields.get('drm-pdev'), client)] = fields
    result = {'cpu_seconds': cpu}
    for engine in ('enc', 'vpe'):
        values = [int(f[f'drm-engine-{engine}'].split()[0]) for f in clients.values()
                  if f'drm-engine-{engine}' in f]
        result[f'drm_engine_{engine}_ns'] = sum(values) if values else None
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("model")
    parser.add_argument("model_info")
    parser.add_argument("--seconds", type=int, default=3600)
    parser.add_argument("--output", type=Path, default=Path("artifacts/soak"))
    args = parser.parse_args()
    if not 15 <= args.seconds <= 86400:
        parser.error("seconds must be 15..86400")
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output / "events.jsonl").exists():
        parser.error("output already contains events.jsonl; select a new output directory")
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(root / "build/src/gst/libamdva_copy_audit.so")
    command = [str(root / "build/src/gst/amdva-run"), args.input, args.model,
               args.model_info, f"--duration={args.seconds}",
               f"--events={args.output / 'events.jsonl'}"]
    samples = []
    start = time.monotonic()
    resource_failure = None
    baseline = None
    with (args.output / "run.json").open("w") as result, \
            (args.output / "run.log").open("w") as log, \
            (args.output / "resources.csv").open("w") as resource:
        process = subprocess.Popen(command, env=env, stdout=result, stderr=log)
        writer = csv.DictWriter(resource, fieldnames=["elapsed_s", "rss_kib", "fds", "gpu_alloc_kib"])
        writer.writeheader()
        try:
            while process.poll() is None:
                elapsed = time.monotonic() - start
                try:
                    status = Path(f"/proc/{process.pid}/status").read_text()
                    rss = next(int(line.split()[1]) for line in status.splitlines()
                               if line.startswith("VmRSS:"))
                    fds = len(list(Path(f"/proc/{process.pid}/fd").iterdir()))
                    sample = {"elapsed_s": round(elapsed, 2), "rss_kib": rss, "fds": fds,
                              "gpu_alloc_kib": gpu_alloc_kib(process.pid)}
                    samples.append(sample)
                    writer.writerow(sample)
                    resource.flush()
                    if elapsed >= min(60, args.seconds / 3):
                        if baseline is None:
                            baseline = sample
                        for key, limit in (("fds", 4), ("rss_kib", 65536), ("gpu_alloc_kib", 65536)):
                            if sample[key] is None or baseline[key] is None or sample[key] - baseline[key] > limit:
                                resource_failure = f"missing counter or excessive growth: {key}"
                                break
                        if resource_failure:
                            process.terminate()
                            try:
                                process.wait(timeout=10)
                            except subprocess.TimeoutExpired:
                                process.kill()
                            break
                except (FileNotFoundError, ProcessLookupError, StopIteration):
                    pass
                if elapsed > args.seconds + 30:
                    process.kill()
                    break
                time.sleep(5)
        except BaseException:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
            raise
        returncode = process.wait()
    elapsed = time.monotonic() - start
    # Warm-up allocations and kernel caches are excluded from growth checks.
    stable = [sample for sample in samples if sample["elapsed_s"] >= min(60, args.seconds / 3)]
    if stable:
        fd_growth = max(s["fds"] for s in stable) - stable[0]["fds"]
        rss_growth = max(s["rss_kib"] for s in stable) - stable[0]["rss_kib"]
        gpu_growth = (max(s["gpu_alloc_kib"] for s in stable) - stable[0]["gpu_alloc_kib"]
                      if all(s["gpu_alloc_kib"] is not None for s in stable) else None)
    else:
        fd_growth = rss_growth = gpu_growth = None
    run = json.loads((args.output / "run.json").read_text()) if returncode == 0 else {}
    passed = bool(returncode == 0 and not resource_failure and elapsed >= args.seconds and stable
                  and fd_growth <= 4 and rss_growth <= 64 * 1024
                  and gpu_growth is not None and gpu_growth <= 64 * 1024
                  and run.get("pipeline_zero_copy_verified") and run.get("dlstreamer"))
    summary = {"schema": 1, "requested_seconds": args.seconds, "elapsed_seconds": elapsed,
               "returncode": returncode, "samples": len(samples), "fd_growth": fd_growth,
               "rss_growth_kib": rss_growth, "passed": passed,
               "gpu_alloc_growth_kib": gpu_growth, "resource_failure": resource_failure,
               "limits": {"fd_growth": 4, "rss_growth_kib": 65536, "gpu_alloc_growth_kib": 65536}, "pipeline": run}
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary), flush=True)
    return 0 if passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
