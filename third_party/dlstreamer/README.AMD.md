# Vendored DL Streamer subset

Source: https://github.com/open-edge-platform/dlstreamer, tag `v2026.2.0`,
commit `ad3f68259b650f1c1aaec694d80cdfcd1055045a`. License: MIT, see `LICENSE`
(Copyright Intel Corporation); third-party notices in `third-party-programs.txt`.

Included: `gvatrack` (with its VAS object tracker), `gvametaconvert`,
`gvametapublish` (file, MQTT, Kafka), `gvastreammux`, `gvastreamdemux`,
`gvametaaggregate`, and the metadata/utility sources they need. Element names
are unchanged so DL Streamer pipelines keep working. Not included: inference
elements (`gvadetect`, `gvaclassify`, `gvainference`), `gvawatermark`,
`gvapython`, OpenVINO and the Python bindings. Use the `amd*` elements instead.

Local changes (applied in-tree; they were `patches/dlstreamer-*.patch`):
- OpenCV includes narrowed to `core.hpp`/`imgproc.hpp`; `label_id` handling in
  `gvatrack` and a label leak fix in `jsonconverter.cpp`.
- `gvametaaggregate` calls the `amd-analytics-merge` transform for metas tagged
  `amd-analysis`.
- `gvastreammux` container mode replays each stream's `STREAM_START` event.

The GstAnalytics group/keypoint API these sources use comes from
`patches/gst-plugins-bad-0020-gst-analytics.patch` (also from DL Streamer;
modified gst-plugins-bad files stay LGPL-2.1-or-later).

Built by the `AMDVA_BUILD_GVA_COMPAT` option into `plugins/` (`libgstgva.so`,
`libgstgvametapublish*.so`) and `libamdva_gva_common.so`.
