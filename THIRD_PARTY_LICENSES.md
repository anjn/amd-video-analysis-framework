# External dependencies

Dependencies are downloaded into ignored `.deps/`, not relicensed as project code.
Retain their license files when packaging a binary distribution.

| Component | Version | License/source |
| --- | --- | --- |
| DL Streamer | v2026.2.0, selected components with local patch | MIT, upstream LICENSE |
| GStreamer | 1.28.2 plus DL Streamer analytics patch | LGPL-2.1-or-later; retain patch/source notices |
| ROCm HIP / MIGraphX | 7.2.0 / 2.15.0 | Component-specific licenses distributed in ROCm packages |
| OpenVINO | 2026.2.0 | Apache-2.0, wheel notices |
| OpenCV | Ubuntu 4.6.0 | Apache-2.0, Ubuntu copyright files |
| libva / Mesa | host libva 2.20 / Mesa 25.2.8 | Upstream and Ubuntu component notices |
| Qt | Ubuntu 6.4.2 | LGPL-3.0 / GPL-2.0 / GPL-3.0; retain Ubuntu copyright and Qt notices |
| SQLite | Ubuntu 3.45.1 | Public domain; retain Ubuntu packaging notices |
| Ultralytics / PyTorch / ONNX | isolated model export environment | Upstream AGPL-3.0 / BSD-style / Apache-2.0 notices; not linked into the NVR runtime |
| nlohmann JSON | 3.11.3 | MIT |
| GLib, json-glib, TBB, build tools | locked packages and host prerequisites | Upstream/Ubuntu license files |

The existing YOLOv8 model and Ultralytics bus test image were used only in ignored
local validation artifacts. They are not included under this repository's MIT
license. Consult the model/image provider's terms before distributing them.
