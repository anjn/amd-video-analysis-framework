<!-- source: examples/resource-dashboard/README.ja.md 17115b1a55338729207a84163ecc2560cc3af8a2 -->
# Resource dashboard

[日本語](README.ja.md)

A sample that renders the APU's CPU, GPU, memory, power and video engine utilization on the GPU, and serves hardware-encoded H.264 over RTSP. It uses no browser, Qt or inference model.

```bash
source scripts/env.sh
build/examples/resource-dashboard/amdva-dashboard --power-max-w 48
```

When `Dashboard ready: rtsp://...` is printed, open `rtsp://<this PC's IP>:8554/dashboard` in an RTSP player such as VLC, or add it as a camera in the NVR demo. There is no authentication, so specify `--bind 127.0.0.1` if you use it only on this PC. `--power-max-w` is the display upper limit (W) of the power bar.

To preview the screen design with mock data, open [index.html](index.html) in a browser. For options, metric sources and validation results, see the [resource dashboard developer documentation](../../docs/dev/resource-dashboard/README.md).
