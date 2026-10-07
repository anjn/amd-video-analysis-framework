# Framework validation

The framework tests add task coverage to the original single-stream and NVR
checks. Reports apply to the validated gfx1150 / ROCm 7.2 / Mesa 25.2.8 host,
not to every AMD APU or every ONNX model. Recorded results are in
[framework-validation-results.json](framework-validation-results.json). Model weights, compiled binaries,
images and per-frame logs remain in ignored `artifacts/`.

## Recorded run

The recorded run passed all 14 CTest checks, the audited complete pipelines and
five independent CPU model comparisons. The 3,600-second NVR run used 20 RTSP
sources, 16 selected views and four background slots, with 100 dynamic control
cycles and a server restart/codec swap. All 1,307 finalized MKV files passed
FFprobe; recovery took 7.0 seconds; copy violations stayed at zero. Maximum
growth after warmup was 23,144 KiB RSS, 34,656 KiB GPU allocation and two FDs.
Observed median display/AI rates were 29.883/5.966 FPS per selected source,
99.999%/99.805% of the recorded baseline, exceeding the 90% acceptance threshold.
Other validation workloads overlapped portions of the run, so these rates are
observations rather than an isolated throughput benchmark.

The one-hour run used a frozen runtime captured before the final stream-flush,
overlay-label and validation hardening changes. Its binary hashes are recorded
in the report. The final build separately passed a 90-second NVR regression
with restart, codec swap, dynamic controls, tracking and file publication, plus
all contract and audited pipeline tests using the installed DL Streamer patches.
The headless build ran two real RTSP sources without Qt/display access, and an
external application built and ran against the installed SDK.

## Contract tests

```bash
source scripts/env.sh
python3 scripts/setup_model_export.py
.deps/model-export/bin/python tools/prepare_test_models.py artifacts/test-models
cmake -S . -B build -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT" \
  -DAMDVA_HARDWARE_TESTS=ON -DAMDVA_DLSTREAMER_TESTS=ON \
  -DAMDVA_TEST_MODELS="$PWD/artifacts/test-models"
cmake --build build -j6
ctest --test-dir build --output-on-failure
```

CPU/API tests cover strict caps, source/config/catalog behavior, metadata
ownership, Python pass/drop/custom/exception/type/size behavior, publication
queue limits and destination recovery. Publication tests use a loopback MQTT
protocol fixture and librdkafka's local mock cluster; no external broker is
contacted. They verify QoS1 acknowledgements and delivery callbacks, not
production broker authentication/TLS configurations or exactly-once behavior.

GPU tests cover named multiple-input/output execution and retained outputs after
another evaluation/model teardown; batch sizes 1/2/4/8/16, sharing, partial
waits, cancellation and queue overflow; classification softmax/top-k;
class-aware NMS; semantic argmax; instance prototype reconstruction; keypoints;
ROI inverse geometry and NV12 alignment; multistage VPE BGR/mean/std reference;
metadata merge ID remapping and GPU payload lifetime; actual overlay pixels;
and permitted/forbidden copy-audit calls.

## Representative models and complete pipelines

Export/compile the models described in [framework usage](framework.md). Keep a
1920x1080 H.264 fixture and the NVR YOLOv8n B1/B4 artifacts from the same weights.
Create a second, smaller stream for the heterogeneous mux test:

```bash
ffmpeg -i "$VIDEO" -frames:v 1 -vf scale=640:360 -c:v libx264 \
  -pix_fmt yuv420p -color_primaries bt709 -color_trc bt709 -colorspace bt709 \
  "$MODELS/small.h264"
build/src/gst/framework-pipeline-tests "$VIDEO" "$MODELS" "$NVR_MODELS"
```

The pipeline test checks detection → tracking → ROI classification → Python →
GPU watermark, pose, semantic segmentation, a tee/metadata-aggregate instance
segmentation branch, and heterogeneous mux → B4 inference → demux. It verifies
original image GstMemory/PTS before drawing, AMD attachment retention, ROI
parent links, custom Python metadata, both mux sources and output lifetime after
pipeline teardown. A flush/stream-replacement test switches between independent
VA displays and resolutions and verifies fresh source identity, generation, PTS
and original image memory. Each demux branch has its own queue to avoid sink preroll
blocking the other source. Paths are quoted for GStreamer's parser.

Enable that test in CTest by setting `AMDVA_ANALYTICS_MODELS`,
`AMDVA_DETECTION_MODELS` and `AMDVA_TEST_VIDEO`. After warmup, run the same
pipeline test with the copy auditor:

```bash
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/src/gst/framework-pipeline-tests "$VIDEO" "$MODELS" "$NVR_MODELS"
```

The test enables auditing after the first completed frame, avoiding model/kernel
initialization while still covering subsequent inference, ROI processing,
tracking, metadata, drawing and output. Auditor counters must stay at zero
violations across all pipelines.

The offline validator deliberately downloads prepared tensors and model outputs
for independent ONNX Runtime CPU comparisons:

```bash
for name in resnet18 yolov8n-seg yolov8n-pose segformer-b0; do
  build/src/gst/amdva-validate-model "$VIDEO" "$MODELS/$name.mxr" \
    "$MODELS/$name.json" "$MODELS/$name-reference"
done
build/src/gst/amdva-validate-model "$VIDEO" "$NVR_MODELS/yolov8n-b1.mxr" \
  "$NVR_MODELS/model-info.json" "$MODELS/yolov8n-reference"
.deps/model-export/bin/python tools/validate_models.py "$MODELS" "$NVR_MODELS" \
  --output="$MODELS/reference-results.json"
```

The CPU model consumes the exact VPE/HIP-prepared tensor, separating inference
error from scaler/filter differences. An independent constant-color BT.709
preprocessing reference tests scaling, chroma-aligned crops, BGR order and
normalization separately. FP16 raw outputs must have relative L2 error below
1%; classification top-k must agree; confident objects must match class,
confidence and IoU; semantic pixel agreement must exceed 99%; instance mask IoU
must exceed .95; confident pose keypoints must stay within five source pixels.
These are fixture correctness checks, not an accuracy benchmark on ImageNet,
COCO or ADE20K datasets.

## NVR and SDK regressions

Run the original NVR soak with 20 recorded sources, 16 selected sources and four
background slots, 100 add/remove/select cycles, a server restart at 600 seconds
and codec swaps. `--build-dir` accepts a frozen build tree so development cannot
replace the libraries used by the one-hour run. Its `LD_LIBRARY_PATH` prioritizes
that tree; dependencies must also stay unchanged while running. Resource guards
remain 64 MiB RSS/GPU growth after warmup and the existing per-channel FD budget.
FFprobe validates finalized MKV files and the copy auditor must report zero
violations. A second run enables file publication and checks selected tracked
observations, untracked background results and source identity.

Validate packaging by installing to a temporary prefix and building
`examples/sdk` with `AMDVideoAnalysis_DIR` set to that prefix. A separate
`AMDVA_BUILD_NVR=OFF` build must succeed without Qt and its `amdva-streams`
executable must process real RTSP sources without a display server. The test
report records these results separately from GUI presentation evidence.
