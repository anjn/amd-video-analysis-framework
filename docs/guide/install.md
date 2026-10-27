<!-- source: docs/guide/install.ja.md c20dfc352cb3b9ff6c2f942b0589f79e0033b921 -->
# Installation

Running the bootstrap script once installs pinned versions of GStreamer and ROCm into `.deps/` inside the repository, builds the framework and runs the tests. It does not use sudo and does not replace any system libraries. Installation is complete after you run one GPU check at the end.

## Requirements

| Item | Validated environment |
| --- | --- |
| Hardware | Ryzen AI 9 HX 370 / Radeon 890M (gfx1150, VPE 6.1.0) |
| OS | Ubuntu 24.04, kernel 6.17.0-1032-oem or later, Mesa 25.2.8 |
| Devices | A user with access to `/dev/dri/renderD128` and `/dev/kfd` |
| Tools | Python 3.12 or later, `uv`, a C/C++ compiler, CMake 3.22 or later, pkg-config, git, dpkg-deb, apt-get, m4, GLib development files (2.64 or later) |
| Disk | The ROCm packages alone are several GB to download, and they take even more space once extracted |

Other AMD APUs and GPUs have not been validated.

## 1. Build the dependencies and the framework

Run this from the repository root. Most of the time is spent downloading and building the dependencies.

```bash
python3 scripts/bootstrap.py --jobs 8
```

ctest runs at the end. The build succeeded if you see the following.

```text
100% tests passed, 0 tests failed out of 26
```

If tests fail, first check the required tools listed in the requirements table and your access permissions on the device files.

## 2. Set the environment variables

In any shell that uses the built framework and dependencies, run the following first, every time. All commands on the following pages assume this setup.

```bash
source scripts/env.sh
```

## 3. Verify the GPU

Check that VA, VPE and HIP work together while sharing the same GPU memory.

```bash
build/amdva-probe
```

The check succeeded if the last line is JSON that contains `"gate_passed":true`.

```text
{"schema":1,"gpu":"gfx1150",...,"vpe_observed":true,...,"gate_passed":true}
```

If it fails, check that you are running it in a shell where you ran `source scripts/env.sh`. If your login environment sets `HSA_OVERRIDE_GFX_VERSION`, HIP runs as a different GPU; `scripts/env.sh` unsets this variable.

You can also check that GStreamer can see the framework's elements.

```bash
gst-inspect-1.0 amdanalytics
```

The check succeeded if the list includes `amddetect`, `amdclassify`, `amdinference`, `amdwatermark`, `amdencode`, `amdpython`, `amdmetapublish` and others.

Next, draw detection results on a video in the [Quick start](quickstart.md).
