# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A GStreamer-based video analytics framework for AMD APUs (validated on Ryzen AI 9 HX 370 / Radeon 890M, gfx1150, Ubuntu 24.04). Decode, preprocessing (VPE), inference (MIGraphX via HIP), drawing and encoding stay on the GPU; only compact metadata returns to the CPU. C++17, CMake.

## Build and test

All dependencies (GStreamer 1.28.2 with patches, ROCm 7.2.0 / MIGraphX 2.15.0, Qt 6, etc.) are installed rootless under `.deps/` by the bootstrap, pinned in `dependencies.lock.json`. **Always `source scripts/env.sh` first**; without it CMake configure fails (e.g. "gstreamer-1.0>=1.28.2 not found") and binaries cannot find their libraries or GStreamer plugins.

```bash
python3 scripts/bootstrap.py --jobs 8        # first time: deps + configure + build into build/
source scripts/env.sh
cmake --build build -j$(nproc)               # or --target <name>, e.g. amdva-nvr
ctest --test-dir build --output-on-failure   # all contract tests
ctest --test-dir build -R nvr-config-contract --output-on-failure   # single test
```

- Building needs the validated AMD host; a machine without `.deps` (e.g. WSL) cannot build.
- Tests are named `*-contract`. Each is a plain `main()` executable with a `check(cond, message)` helper; no test framework. Register new ones next to the code (`add_executable` + `add_test` in the nearest `CMakeLists.txt`); Qt widget tests set `QT_QPA_PLATFORM=offscreen` via `set_tests_properties`.
- Tests that need the real VA/HIP device are only registered with `-DAMDVA_HARDWARE_TESTS=ON`.
- Useful CMake options: `AMDVA_BUILD_NVR_GUI` (Qt desktop), `AMDVA_BUILD_NVR`, `AMDVA_BUILD_PLUGINS`, `AMDVA_BUILD_STREAMING`, `AMDVA_WITH_MQTT/KAFKA/PYTHON`.
- Don't reinstall or rebuild libraries while a pipeline is running.
- Model artifacts (`.mxr`) are compiled with `build/src/gst/amdva-compile-model` and carry a SHA256/GPU manifest that is checked at runtime; see `docs/dev/host-build.md`.

## Architecture

Libraries (public headers in `include/amdva/`, exported as `AMDVA::` targets):

| Target | Source | Role |
| --- | --- | --- |
| `amdva_runtime` | `src/runtime.cpp`, `preprocess.cpp`, `telemetry.cpp` | VA/HIP import, VPE ops, GPU image packing, decode/presentation timing |
| `amdva_inference` | `src/inference.cpp`, `model.cpp`, `postprocess.cpp`, `batching.cpp` | MIGraphX models with GPU tensor I/O, manifests, task adapters, `SharedBatchExecutor` |
| `amdva_gst_support` | `src/gst/*` (non-plugin parts) | GstAnalytics interop, tracking, overlay, result publishing |
| `gstamdanalytics` | `src/gst/plugin.cpp` … | GStreamer elements (`amdpreproc`, `amdmigraphx`, `amdwatermark`, `amdmetapublish`, …) |
| `amdva_media` | `src/media` | H.264 VUI, RTSP server wrapper, VA→EGL DMA-BUF import |
| `amdva_streaming` | `src/streaming` | RTSP ingest, segment recording, SQLite catalog with quota, playback, preview surfaces, transcoding (no Qt) |
| `amdva_monitor` | `src/resource_monitor.cpp` | CPU/GPU/memory/power and DRM engine utilization |

`third_party/dlstreamer` vendors DL Streamer elements (`gvatrack`, `gvametaconvert`, …); inference does not use OpenVINO.

Hard rules from `docs/dev/architecture.md` that code changes must respect:
- No decoded pixels go to the CPU and there is no silent CPU or shader fallback. Failures become bus errors. `tools/copy_audit.cpp` (`libamdva_copy_audit.so`, used via `LD_PRELOAD`) enforces this and allows only bounded, explicitly marked metadata transfers.
- VA surface IDs are only valid on the GstVaDisplay that created them. Preprocessors retain the decoder's display.
- Inputs are NV12 VAMemory, BT.709 limited range, even dimensions.

`examples/` are built only on the public headers and `AMDVA::` targets. The main one is **`examples/nvr`**, a 16-camera NVR demo:
- `backend/`: the Qt-free `amdva_nvr_backend` library (`namespace amdva::nvr`).
  - `Controller` (`controller.cpp`) owns the camera channels, the shared VA display, 16 GPU slots and the GPU/preview/control threads.
  - `AiRuntime` (`ai.hpp`) schedules GPU work by priority lease and records per-stage wall-time metrics (`record(stage, camera, ms, wait, batch)`; per-camera rows are also aggregated into the `""` camera row).
  - `SearchService` (`search.cpp`) runs person tracking (ReID), image embeddings (EmbeddingGemma or MobileCLIP2) and vector search, with data stored in the recording catalog DB.
  - MobileCLIP on the NPU runs in a separate worker process (`mobileclip_worker.cpp`) inside the Ryzen AI venv (`search.ryzen_ai_venv`). The framework itself has no NPU backend.
- `app/`: Qt desktop `amdva-nvr`. `vms.cpp` builds the tabs; `ai_page.cpp` / `ai_overview.cpp` are the AI Statistics tab; `window.cpp` is the EGL video wall.
- `headless.cpp`: `amdva-streams`, which is the backend without a display.
- `tools/`: validation programs (`amdva-ai-validate`, `amdva-nvr-validate`, …) that write JSON records to `docs/dev/nvr/*-results.json`.
- Configuration example: `examples/nvr/config/nvr.json`. Run the app with `build/examples/nvr/amdva-nvr <config.json>` (`--fullscreen` for demos).

## Documentation conventions

- Pages come in pairs: `x.md` (English) and `x.ja.md` (Japanese).
  - The originals of `docs/guide/` and the READMEs are **Japanese**.
  - The originals of `docs/dev/` are **English**.
- A translation starts with `<!-- source: <path> <blob hash> -->`. After editing an original and updating its translation, run `python3 scripts/docs_site.py --stamp <translation>`.
- CI (`.github/workflows/docs.yml`) runs `python3 scripts/docs_site.py --check-translations` and `mkdocs build --strict`.

## Commits

Commit messages are a single imperative, sentence-case summary without type prefixes (e.g. "Batch person tracking jobs under one GPU lease and one ReID call"), followed by an explanatory body when useful.
