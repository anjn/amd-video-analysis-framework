# Demo quality fixtures

`person-review.json` records 53 visible person boxes in sixteen M01–M16 museum
video frames at 10 seconds, viewed at 960 × 540. Boxes shorter than 54 pixels
are excluded. Initial detections were visually reviewed, with obvious artwork,
stanchion and duplicate detections removed. Heavily occluded/unresolvable people
are not exhaustively annotated; the detector-seeded set can favor the baseline.

`appearance-review.json` records 52 reviewed crops and appearance labels for
small-set retrieval checks. Ambiguous overlapping crops were excluded. These
labels do not establish a production cross-camera identity feature. Neither
file contains temporal trajectories suitable for tracking IDF1.

The source videos remain local in `~/work/videos`. Recreate frames, crops and
independent 30-second 1080p RTSP fixtures with `examples/nvr/scripts/prepare_ai_fixture.py`.
The default output is ignored under `artifacts/ai-benchmark`; original videos
are never overwritten. Quality scripts require the local model-export Python
environment. See [AI performance](../../../docs/dev/nvr/nvr-ai-performance.md) for execution
and interpretation. Model checkpoints and compiled models are not checked in.

The user reports 40 people in total across the original sixteen 10-minute
videos. The reviewed subsets do not annotate all 40. Integration load tests
use 30-second excerpts, so their loop boundaries can produce extra track IDs.
IDs are camera-local; do not compare the summed ID count with 40 as an identity
accuracy metric.
