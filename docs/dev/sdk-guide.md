# SDK guide

## Libraries

| Target | Headers | Contents |
| --- | --- | --- |
| `AMDVA::amdva_runtime` | `runtime.hpp`, `preprocess.hpp`, `telemetry.hpp` | VA/HIP import (`VaSession`, `ImportedSurface`), VPE operations, image packing |
| `AMDVA::amdva_inference` | `model.hpp`, `batching.hpp`, `inference.hpp` | `ModelInfo`, `ModelExecutor`, task post-processors, shared batch executor |
| `AMDVA::amdva_gst_support` | `tensor_meta.hpp`, `video_preprocess.hpp`, `tracking.hpp`, `overlay.hpp`, `publishing.hpp`, `gst_telemetry.hpp` | GstAnalytics interop, tracking, drawing, publication |
| `AMDVA::amdva_media` | `vui.hpp`, `rtsp_server.hpp`, `egl_import.hpp`, `copy_audit.hpp` | encode helpers, RTSP serving, EGL import |
| `AMDVA::amdva_streaming` | `streaming/*.hpp` | `RtspIngest`, `SegmentRecorder`, `Catalog` (recordings, recorded analysis, application tables), playback sessions, `PreviewSlot` |
| `AMDVA::amdva_monitor` | `resource_monitor.hpp` | CPU/GPU/memory/power and DRM engine load |

Shared libraries carry the SONAME `lib<name>.so.<major>.<minor>`; 0.x minor
releases make no ABI promise, so rebuild applications when the minor version changes.

Plugins (`gstamdanalytics`, `gstgva`, `gstgvametapublish*`) install to
`lib/gstreamer-1.0`. Add that directory to `GST_PLUGIN_PATH` when you use a
non-system prefix.

## Build and install

```bash
source scripts/env.sh                 # rootless dependency environment
cmake -S . -B build -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8
cmake --install build --prefix /path/to/amdva-sdk
```

Useful options: `AMDVA_BUILD_PLUGINS`, `AMDVA_BUILD_GVA_COMPAT` (vendored DL
Streamer elements), `AMDVA_BUILD_STREAMING`, `AMDVA_HARDWARE_TESTS`, and for the
applications under `examples/`: `AMDVA_BUILD_EXAMPLES`, `AMDVA_BUILD_NVR`,
`AMDVA_BUILD_NVR_GUI` (needs Qt6), `AMDVA_BUILD_DASHBOARD`, `AMDVA_BUILD_RTSP_FILES`.

Optional dependencies, all `ON` by default: `AMDVA_WITH_MQTT` (Eclipse Paho),
`AMDVA_WITH_KAFKA` (librdkafka) for result publication, and `AMDVA_WITH_PYTHON`
(embedded CPython) for `amdpython`. A minimal framework build:

```bash
cmake -S . -B build-min -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT" -DAMDVA_BUILD_EXAMPLES=OFF \
  -DAMDVA_WITH_MQTT=OFF -DAMDVA_WITH_KAFKA=OFF -DAMDVA_WITH_PYTHON=OFF
```

`AMDVA_SYSROOT` (default `.deps/sysroot`) locates the rootless development
packages. The installed package provides `amdva_use_host_glib()` and
`amdva_use_host_sqlite()` (`AmdvaHostLibraries.cmake`) so applications link
the host GLib/SQLite runtime like the framework does.

## Consuming from CMake

```cmake
find_package(AMDVideoAnalytics REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE AMDVA::amdva_media)
```

`examples/sdk/CMakeLists.txt` is a standalone project:

```bash
cmake -S examples/sdk -B build-sdk -DCMAKE_PREFIX_PATH=/path/to/amdva-sdk
```

## Examples

| Program / script | Shows |
| --- | --- |
| `examples/sdk/transcode.cpp` | decode, detect, overlay, encode to a file or RTSP |
| `examples/nvr/headless.cpp` | headless NVR service on the sample's controller (RTSP ingest, recording, AI) |
| `examples/nvr`, `examples/resource-dashboard`, `examples/rtsp-files` | complete applications; see each README |
| `examples/sdk/playback.cpp`, `timeline.cpp`, `keyframe-preview.cpp` | recorded-video APIs |
| `examples/sdk/telemetry.cpp` | decode and presentation timing |
| `examples/pipelines/*.sh` | `gst-launch` recipes: annotate to file, segment, multistream |
| `examples/metadata_callback.py` | `amdpython` callback |

## Tests

```bash
ctest --test-dir build                        # no GPU needed
cmake -S . -B build -DAMDVA_HARDWARE_TESTS=ON \
  -DAMDVA_DETECTION_MODELS=/dir/with/yolov8n-b1.mxr+model-info.json \
  -DAMDVA_TEST_VIDEO=/path/test.h264
ctest --test-dir build -R "encode|framework"  # needs the AMD GPU
```

Hardware tests include `encode-contract` (H.264/H.265/AV1, NV12 and BGRA input,
BT.709 signalling) and `encode-annotated-contract` (decode, detect, overlay,
encode through the DMA-BUF path).

## Dependencies and licenses

`scripts/bootstrap.py` builds a locked GStreamer 1.28.2 with the GstAnalytics
patch and a rootless sysroot under `.deps/`. DL Streamer sources are vendored
in `third_party/dlstreamer` (MIT). See `THIRD_PARTY_LICENSES.md`.
