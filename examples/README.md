# Examples

Applications built on the framework. They use only the installed headers and
`AMDVA::` CMake targets, so each directory also configures on its own against an
installed SDK (`cmake -S examples/<name> -B build-<name> -DCMAKE_PREFIX_PATH=/path/to/amdva-sdk`).
In the source tree they build with the framework unless `-DAMDVA_BUILD_EXAMPLES=OFF`.

| Example | Shows |
| --- | --- |
| [NVR](nvr/README.md) | 16-camera recorder: Qt-free controller, person search, headless `amdva-streams` service and Qt desktop |
| [Resource dashboard](resource-dashboard/README.md) | GPU-rendered, hardware-encoded RTSP dashboard of CPU/GPU/memory/power readings |
| [File-to-RTSP server](rtsp-files/README.md) | Serves MP4/MKV files as looped RTSP cameras for testing |
| [SDK programs](sdk) | Transcode, decode/presentation telemetry, recording playback, timeline and keyframe previews |
| [gst-launch recipes](pipelines/README.md) | Annotate to a file, record segments, multistream encode |
| [`yolov8n.json`](yolov8n.json), [`metadata_callback.py`](metadata_callback.py) | Model-info for YOLOv8n and an `amdpython` callback |
