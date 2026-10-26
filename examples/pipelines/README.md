# gst-launch recipes

Source `scripts/env.sh` first (or install the package and set `GST_PLUGIN_PATH`
to its `lib/gstreamer-1.0`). Every script reads its inputs from environment
variables and prints the command it runs.

| Script | What it shows |
| --- | --- |
| `annotate-to-file.sh` | decode, detect, overlay, hardware encode to MP4/MKV |
| `annotate-segments.sh` | the same stream cut into fixed-length files with `splitmuxsink` |
| `multistream-encode.sh` | two sources through one shared detector (`gvastreammux`/`gvastreamdemux`), each annotated and encoded separately |

Common variables: `VIDEO` (H.264 elementary stream or MP4), `MODEL` (`.mxr`),
`MODEL_INFO` (model-info JSON), `CODEC` (`h264`, `h265` or `av1`, default `h264`),
`BITRATE` (kbit/s, default 6000).

The frame path is `vah264dec -> VAMemory NV12 -> amddetect -> amdwatermark
(BGRA DMA-BUF) -> amdencode (VA video processor -> VAMemory NV12 -> vah26xenc)`.
No raw image is mapped on the CPU. A raw `.h264` file carries no timestamps, so
the scripts add `video/x-h264,framerate=30/1`; drop that capsfilter for MP4/RTSP
sources that already carry timing.
