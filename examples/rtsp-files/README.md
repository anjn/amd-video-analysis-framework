<!-- source: examples/rtsp-files/README.ja.md b579035ab4f3f78d1129030941e1c891a101bfe7 -->
# File RTSP server

[日本語](README.ja.md)

A test tool that serves video files in place of RTSP cameras. It assigns one URL per file and loops back to the start when it reaches the end. Video is neither decoded nor re-encoded, so it does not use the GPU.

```bash
source scripts/env.sh
build/examples/rtsp-files/amdva-rtsp-files --port 8558 /path/to/a.mp4 /path/to/b.mkv
```

URLs such as `rtsp://127.0.0.1:8558/cam0` and `rtsp://127.0.0.1:8558/cam1` are printed in input order. Supported inputs are H.264/H.265 in MP4/MOV and Matroska, and raw H.264/H.265 streams. To pass the list of files in a text file such as [files.txt](files.txt), use `--list`. For options and validation results, see the [file RTSP server developer documentation](../../docs/dev/rtsp-files/README.md).
