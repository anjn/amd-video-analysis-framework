<!-- source: README.ja.md ed3aac938cdc53cec89a49c57ce22e473ef846e7 -->
# AMD Video Analytics Framework

[日本語](README.ja.md)

A GStreamer-based video analytics framework for AMD APUs. Decoding of camera feeds and video files, inference such as object detection, drawing of detection results, and encoding all run on the GPU; only metadata such as detection results returns to the CPU. Validated on Ryzen AI 9 HX 370 / Radeon 890M.

## Demo

<!-- Place the NVR demo video here -->

For details and launch instructions of the NVR demo, which records multiple cameras while displaying detection results, see the [NVR demo](docs/guide/nvr-demo.md).

## Features

- **Processing that stays on the GPU**: Pixels are never copied to the CPU between decoding, preprocessing, inference, drawing and encoding.
- **Built from GStreamer elements**: Use object detection, classification, instance/semantic segmentation and pose estimation in `gst-launch-1.0` pipelines.
- **Bundled DL Streamer elements**: Tracking (`gvatrack`), multi-stream batching and metadata conversion are available under the same element names as in DL Streamer.
- **Result output**: Publish detection results to JSON Lines, MQTT or Kafka. Annotated video is hardware-encoded and can be saved to a file or served over RTSP.
- **C++ SDK**: Embed RTSP camera ingest, recording, and recording search and playback into your application without depending on GUI libraries.
- **Samples**: A 16-camera NVR demo, a GPU-rendered resource dashboard, and a tool that serves video files as RTSP cameras.

## Requirements

| Item | Validated environment |
| --- | --- |
| Hardware | Ryzen AI 9 HX 370 / Radeon 890M (gfx1150) |
| OS | Ubuntu 24.04, kernel 6.17.0-1032-oem or later, Mesa 25.2.8 |
| Dependencies | GStreamer 1.28.2, ROCm 7.2.0 / MIGraphX 2.15.0 (the bootstrap installs them inside the repository) |

## Documentation

The documentation is available at <https://anjn.github.io/amd-video-analytics-framework/>.

- First-time users: [Installation](docs/guide/install.md) → [Quick start](docs/guide/quickstart.md)
- Building pipelines: [Building pipelines](docs/guide/pipelines.md)
- Embedding in applications: [Embedding the SDK in your application](docs/guide/sdk.md)
- Design, all properties, validation results: [Developer documentation](docs/dev/framework.md)

## License

The code in this repository is under the [MIT License](LICENSE). Bundled third-party sources (parts of DL Streamer, sqlite-vec, etc.) are subject to their respective licenses; see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for the list. Model weights and sample images are not included. If you use them, check their respective terms of use.
