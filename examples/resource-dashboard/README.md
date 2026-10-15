# Resource dashboard: visual prototype

Open [index.html](index.html) directly in a browser. No server, installation,
network connection or hardware access is required. All readings, host details
and historical samples are **synthetic**, including the example NVR process.

```bash
xdg-open examples/resource-dashboard/index.html
```

The composition is 1920 × 1080. Narrower windows scale the entire composition
without rearranging panels. For the video composition alone, open
`index.html?video=1`; the **DEMO DATA** badge remains visible. Resize the browser
to compare its readability with NVR tile sizes. This mode does not produce a
video stream.

Use the controls outside the video area to select **Idle**, **CPU load**,
**GPU load** or **Mixed load**, pause/resume, simulate missing GPU metrics, and
show/hide the example process. Scenario changes affect new samples, retaining
the preceding history. Pause freezes both sample collection and the displayed
time; resume continues the simulated timeline. Missing-metric changes take
effect at the next sample, or after resuming if paused.

The preview starts with 60 seconds of generated history and updates every
100 ms. History is bounded to 601 samples and a 60-second window. Missing,
non-finite or absent readings display `N/A` and break graph lines. Large time
gaps also break lines. DRAM graphs use a data-driven scale in GB/s, never a
percentage of an assumed hardware peak. The clock follows sample timestamps
and uses the browser's local timezone.

![Example dashboard with synthetic data](preview.png)

## Metric definitions for a future collector

Metric selection follows the reference repository's
[resource-utilization notes](https://github.com/bowenamd/pi05-pytorch-fast#resource-utilization-cpu--gpu--dram)
and [monitor implementation](https://github.com/bowenamd/pi05-pytorch-fast/blob/main/scripts/monitor_util.py).
These sources are documented here; this prototype does not collect them.

| Display | Intended source / meaning |
| --- | --- |
| CPU utilization | `/proc/stat` deltas; whole-system utilization, 0–100% |
| GPU utilization | amdgpu `gpu_busy_percent`; device-wide, not per-process |
| Host memory used / total | `/proc/meminfo`, `MemTotal − MemAvailable` / `MemTotal` |
| VRAM / GTT allocated | amdgpu `mem_info_vram_used` / `mem_info_gtt_used` |
| DRAM read / write | `gpu_metrics` v3.0 `dram_reads` / `dram_writes`, MB/s converted to decimal GB/s; CPU and GPU combined |
| Socket / APU / GFX power | `gpu_metrics` v3.0 power readings, mW converted to W |
| GFXCLK / FCLK / UCLK | `gpu_metrics` v3.0 clocks, MHz |
| Optional process CPU | Process-tree CPU deltas; 100% means one logical CPU, so values can exceed 100% |
| Optional process RSS | Sum of process-tree RSS; shared pages may be counted more than once |

Memory capacities use binary GiB; bandwidth uses decimal GB/s. APU shared
memory means host usage, RSS, VRAM and GTT are not independent physical pools;
do not add them. GPU metrics are SMU-filtered, approximately one second, even
if sampled every 100 ms. Missing or unsupported metrics must remain unavailable.
The reference machine's 256 GB/s theoretical DRAM bandwidth is not a default
for other APUs. Bandwidth percentages require an explicit machine-specific
theoretical bandwidth; none is assumed or displayed here.

## Data boundary and next stage

Rendering consumes timestamped objects, separate from mock generation.
`window.ResourceDashboard.ingest(sample)` accepts a sample and renders it;
`snapshot()` returns the latest sample and history count. Timestamps are epoch
milliseconds and must increase strictly; invalid or out-of-order timestamps
are rejected. Values are copied into history. `null`, absent and non-finite
metrics are unavailable. The fields are:

```javascript
{
  timestampMs,
  cpuPct, gpuPct, dramReadGBs, dramWriteGBs,
  hostUsedGiB, hostTotalGiB, vramGiB, gttGiB,
  socketW, apuW, gfxW, gfxMHz, fclkMHz, uclkMHz,
  processCpuPct, processRssGiB
}
```

For manual sample injection, pause the demo first. There is no live collector
integration in this version. Machine labels are fixtures in the HTML and must
be replaced with discovered metadata when live collection is implemented.

After visual review, implement real collection and a separate hardware video
sample. This HTML defines the composition; it does not mandate browser capture
as the video source. The next stage must validate GPU-rendered shared surfaces,
GPU color conversion and hardware encoding without CPU pixel readback, then
serve the encoded output over RTSP. This prototype adds no encoder, RTSP server
or changes to the existing NVR application.

## Prototype validation

Checked in Chromium with local `file://` loading: all four scenarios,
pause/resume, missing readings and recovery, process visibility, timestamp
rejection, and 125 seconds of accelerated updates with bounded history.
The 1080p panels fit without clipping. Video-only compositions were checked at
1920, 960, 480 and 320 pixels wide without horizontal overflow. The 480-pixel
preview retains the main values and graph trends; detailed notes require a
larger view. No browser errors or external network requests were observed.
The local validation log is `artifacts/resource-dashboard/validation.json`.
