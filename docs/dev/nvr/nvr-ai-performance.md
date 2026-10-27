# AI scheduling, statistics and 16-camera evaluation

The NVR has an **AI Statistics** tab with a 60-second rolling window. It shows
mean, P50, P95 and maximum completed wall time, average wait, calls per second,
logical batch size, pending work and cumulative drops. The summary combines
cameras; the detail selector exposes individual cameras. The camera table
shows detection/tracking update rates, confirmed people and tracking-frame age.

Detection/tracking can be changed from 0.1 to 10 Hz per camera in 0.1 Hz steps.
Embedding intervals can be changed from 0.1 to 60 seconds per person in
0.1-second steps. Press Enter or leave the field to apply the edit; changes take effect
immediately and are saved to the current configuration. The effective embedding
interval is also shown because it can be longer than the requested interval.

## Execution policy

YOLO and person crop/ReID work share the highest priority, followed by user
queries, then background image embedding. GPU inference sections, including MobileCLIP GPU worker calls, are serialized
with FIFO ordering within a priority. MobileCLIP NPU runs independently. An already-running inference is not preempted.
For EmbeddingGemma 2, after each background embedding batch, the scheduler waits four times that
batch's execution duration before allowing another. This caps collection's
scheduled inference duty at approximately 20%; it is not a GPU-utilization cap
and does not include video decode, rendering or CPU/database work.

MobileCLIP has separate **GPU duty limit** and **NPU duty limit** settings in
AI Statistics (1–100%, default 100%, applied and saved immediately). At 100%
there is no intentional post-inference wait; at 20% the wait is four times that
device's last inference duration. GPU and NPU dispatch independently, without a
paired-batch barrier. The requested per-person interval is retained; MobileCLIP
does not lengthen it using the EmbeddingGemma adaptive budget. GPU collection
still yields to detection/tracking and queries, while NPU does not use the GPU gate.

Each camera is admitted on its requested cadence using its newest frame.
Tracking has at most 16 queued camera frames; a new frame replaces that camera's
older queued frame. Embedding retains the newest high-confidence crop for each
confirmed person, up to `search.queue_limit` (default 128). Old departed-person
entries expire. Overload can drop collection, but does not accumulate an
unbounded backlog. For EmbeddingGemma 2, the effective interval adapts to
estimated active-person cost and is bounded at 60 seconds. New people can receive an initial embedding
before that periodic interval. Five-second coverage is therefore a target,
not a guarantee for 128 simultaneous people.

The tracker retains its existing high/low confidence association and appearance
thresholds. Detection confidence 0.1 remains available for low-score recovery;
new identities still require 0.6 and confirmation. `search.min_person_height`
(default 0.1) excludes people shorter than that fraction of source-frame height
from crop/ReID/search processing. This is appropriate for the requested demo,
where distant small people need not be indexed.

YOLO preprocessing, inference, postprocessing and executor total are separate
rows. **Do not sum overlapping rows**: executor total includes its components,
and detection updates describe scheduling, not extra inference work. A ReID
call may process multiple physical batches; its mean batch column is the number
of real people in that frame. Queue waits include intentional pacing;
per-camera detection/tracking wait values represent frame age. Drop counters
count rejected/replaced work items, not distinct people: one person can have
many candidates skipped while the queue is full. Model loading
and warmup are excluded. These are wall-clock measurements, not GPU event
utilization counters. Samples and row counts are bounded.

## Selected configuration

| Stage | Model/input, width × height | Batch | Reason |
|---|---|---:|---|
| Detection | YOLO11n FP16, 640 × 384 | 1/4 | Better high-confidence recall on reviewed demo frames than YOLOv8n; reduced rectangular input |
| ReID | OSNet x0.25 MSMT17 FP16, 128 × 256 | 4 | Lower inference cost while retaining full person height |
| Semantic image | EmbeddingGemma 2 native ROCm, 108 × 192, token budget 70 | up to 4 | Portrait input and bounded blocking time |

The runtime supports YOLO rectangular dimensions up to 640 on each axis and
ReID batches 1–16 with height 64–256 and width 32–128. It validates the compiled
model's tensor contract. YOLO11 uses the existing YOLOv8-family output decoder.
YOLOX Nano was benchmarked separately with its own decoder; it was not selected
and is not accepted as an interchangeable live-model output format.

Recommended search settings:

```json
{
  "ai_fps": 2,
  "search": {
    "image_size": 256,
    "interval_ms": 5000,
    "queue_limit": 128,
    "embedding_width": 108,
    "embedding_height": 192,
    "embedding_batch": 4,
    "min_person_height": 0.1
  }
}
```

`examples/nvr/config/nvr-ai-16.json` provides a complete 16-source example and uses
`examples/nvr/config/yolo11n-rect.json` for the rectangular detector metadata. Prepare the
referenced models and native bundle before launching it.

These fields supplement the library, bundle and model paths in the normal NVR
configuration. `image_size` bounds the stored crop and ReID source; embedding
gets a separate smaller, aspect-preserving input with black padding and no
upscaling. Both embedding dimensions must be set together. Leaving both zero
preserves the legacy square preprocessing namespace. Collection
intervals down to 100 ms are honored as requested targets. EmbeddingGemma adaptive intervals are
rounded up in 100 ms increments, still constrained by the 20% collection budget.
Changing preprocessing or ReID weights selects a different search namespace;
old observations remain in the catalog and can be queried with their original
configuration. There is no automatic backfill.

Prepare portrait native profiles for batches 1, 2, 4 and 8 using the pinned
`embeddinggemma2-rocm` source and matching ROCm/LibTorch described in
[nvr-search.md](nvr-search.md). Keep text profiles in the bundle. For example:

```bash
eg2 prepare --model /path/to/google --output artifacts/embedding-portrait \
  --budgets 70 --batches 1 2 4 8 --aspect 9 16 --precision fast
```

`examples/nvr/scripts/merge_embedding_profiles.py` combines compatible prepared bundles
using local symlinks. The runtime pads partial image batches by repeating the last valid image and
discards the extra outputs, including for region queries. This keeps one fixed
physical image profile resident: the native library otherwise reloads its
image profile when switching batch shapes. That profile is warmed before
cameras start. Text inference uses a separate profile cache.

## Reproduce measurements

The native benchmark and validation executables require a build configured
with `AMDVA_HARDWARE_TESTS=ON`. Run GPU tests sequentially, with the NVR stopped.
Record the machine's power
settings along with results; timings taken before a power-setting change must
not be used as the final comparison.

```bash
source scripts/env.sh
.deps/model-export/bin/python examples/nvr/scripts/benchmark_ai.py --screen
# Select finalists, then rerun cached models with 20 warmups and
# at least 200 iterations AND 30 seconds, repeated three times:
.deps/model-export/bin/python examples/nvr/scripts/benchmark_ai.py \
  --models yolo11n --sizes 640x384 --batches 1 4 --remeasure
.deps/model-export/bin/python examples/nvr/scripts/benchmark_ai.py \
  --models osnet_x0_25 --sizes 128x256 --batches 1 4 --remeasure
python3 examples/nvr/scripts/benchmark_embedding.py --library /path/to/libeg2.so \
  --bundle /path/to/prepared-bundle --sizes 108x192 --batches 4
```

The detector/ReID benchmark measures model execution only with a resident
input. Native embedding includes preprocessing and output transfer. Full-pipeline
statistics include crop extraction, CPU preprocessing and queueing separately.
`--seconds 3 --repeats 1` is useful for exploratory screening but is not a final
performance measurement. Compiled artifacts and successful results are cached;
new outputs are renamed atomically so an interrupted run can resume.

`examples/nvr/benchmarks/person-review.json` contains 53 visually reviewed boxes from 16
frames. `examples/nvr/benchmarks/appearance-review.json` contains 52 reviewed person crops.
Create independent 1080p copies and quality fixtures from `~/work/videos`:

```bash
.deps/model-export/bin/python examples/nvr/scripts/prepare_ai_fixture.py
```

The user reports 40 people in total across the original sixteen 10-minute
videos. The short load-test excerpts do not establish identity continuity over
the full ten minutes; loop cuts and camera-local IDs can increase the ID count.
The original videos are not modified. The quality scripts compare detector
recall at confidence 0.1/0.5/0.6, ReID retrieval and native FP16 parity, and text
retrieval across embedding input sizes. These are small demo-specific checks,
not exhaustive annotations or a temporal tracking IDF1 benchmark. Formal
IDF1 and a worst-case 16 × 8 simultaneous-person load remain separate validation
requirements. Reducing embedding pixels does not proportionally reduce model
execution because the token budget and native internal profile still matter.

For integration, serve the fixture list and use a dedicated recording root:

```bash
build/examples/rtsp-files/amdva-rtsp-files \
  --list artifacts/ai-benchmark/videos.txt --port 18569
# In another terminal with the native runtime environment configured:
build/examples/nvr/amdva-ai-validate examples/nvr/config/nvr-ai-16.json 1800 \
  artifacts/ai-benchmark/soak.jsonl
```

The validator tests frequency controls and saved settings, runs all-visible,
four-visible and all-hidden camera phases, records five-second JSON snapshots,
and captures the statistics screen at the end. It checks that every camera
continues inference and that semantic observations are saved. Inspect stable
windows of each phase for rate, queue and memory behavior; startup is included
in the log. Use `amdva-search-validate` with a separate two-camera fixture to
check text search, person grouping, region search, inline playback and deletion.

## Measured results on this host

Final isolated measurements on 2026-10-22, after the user reduced the machine's
power setting, used Ryzen AI 9 HX 370 / Radeon 890M (gfx1150), kernel
6.17.0-1033-oem. The exact BIOS power limit was not recorded. Three repeats
produced these average batch times:

| Model/input | Batch | Batch ms | Per image ms |
|---|---:|---:|---:|
| YOLOv8n 640 × 640 baseline | 4 | 9.705 | 2.426 |
| YOLO11n 640 × 384 | 1 | 1.553 | 1.553 |
| YOLO11n 640 × 384 | 4 | 5.549 | 1.387 |
| OSNet x1.0 128 × 256 baseline | 1 | 1.400 | 1.400 |
| OSNet x0.25 128 × 256 | 1 | 0.690 | 0.690 |
| OSNet x0.25 128 × 256 | 4 | 1.206 | 0.302 |
| Embedding 384 × 384 baseline | 1 | 52.394 | 52.394 |
| Embedding 108 × 192 | 4 | 195.666 | 48.917 |

Embedding reduction is only about 7% per image here. Its approximately 196 ms
non-preemptible batch remains the largest inference section, so frequency and
pacing matter more than pixel reduction. At that isolated cost, 128 active
people would require about 31 seconds between embeddings to fit a 20% duty
budget; the adaptive discrete interval would be 60 seconds. This calculation
is an estimate, not a measured 128-person throughput result.

On the reviewed 53-box subset, YOLO11n 640 × 384 matched 52 boxes at confidence
0.1, 51 at 0.5 and 49 at 0.6. The YOLOv8n 640-square baseline matched 53/45/42;
YOLOX Nano matched 45/38/33 and was rejected. Native FP16 YOLO11 reproduced the
52/51/49 counts. Native ReID rank-1 on 52 reviewed crops was 84.6% for selected
OSNet x0.25 versus 82.7% for x1.0. At 64 × 128 the x0.25 result fell to 55.8%,
so the full-height input was retained. With 11 clothing queries, mean semantic
Recall@5 was 82.6% for 108 × 192 versus 66.7% for 384-square; each shape had at
least one relevant hit in the first five results for every query. These small
fixtures support the demo selection but do not establish general accuracy.

Raw repeat statistics and compact quality results are checked in as
[final timings](../../../examples/nvr/benchmarks/ai-final-2026-10-22.json) and
[quality checks](../../../examples/nvr/benchmarks/ai-quality-2026-10-22.json). The broader initial
matrix used short exploratory runs, mostly batches 1 and 4. Detector/ReID
batch-8/16 compilation was interrupted by host restarts and is not represented
as a completed comparison. Batch 4 is the validated live configuration.

## 16-camera integration result

The 1,804-second run completed with ten-minute all-visible, four-visible and
all-hidden phases, checked against actual camera selection and slot state.
In stable windows, every camera's detection and tracking rates stayed between
1.983 and 2.017 Hz. All sixteen cameras produced stored observations. There
were 5,401 cumulative saves; five-minute retention intentionally removes older
rows. Resident memory in stable windows ranged from 6,300 to 6,318 MiB; peak
RSS, including startup, was 7,381 MiB. No camera/search errors or host restarts
occurred in this run. The embedding queue stayed at or below 128; candidate
skips occurred under load and the effective interval was generally 30 seconds.
This does not establish five-second coverage for every person.

The final screenshot exposed an unset decoded-frame timestamp in continuous
hidden-camera analysis. That affected the displayed frame age/wait, not model
execution durations or update counts. The timestamp is now set on decoded
frames, and the validator explicitly rejects stale tracking timestamps. The
long-run report omits the invalid frame-age wait fields for mixed/hidden phases;
a subsequent 120-second validation passed in all three modes, with tracking
ages between 71 and 726 ms. Tracking queue drops were zero throughout the
30-minute run.

See [integration results](../../../examples/nvr/benchmarks/ai-soak-2026-10-22.json). The source
fixture is sixteen 30-second 1080p excerpts, not the full ten-minute originals.
Neither this throughput run nor the reviewed still-image subsets establishes
IDF1 or that all 40 physical people retain one identity throughout the source
videos. Tracking remains camera-local.
