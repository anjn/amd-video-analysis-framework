<!-- source: examples/nvr/README.ja.md 38f8b87365a17cb5b2d1a0212c4f6c7a398ca4fc -->
# NVR demo

[日本語](README.ja.md)

A sample network video recorder that records all registered RTSP cameras while showing up to 16 of them on the desktop, with detection results and tracking IDs drawn as an overlay. Recordings can be played back from a timeline, and you can also try person search. Launch instructions and a description of the screens are in the [NVR demo guide](../../docs/guide/nvr-demo.md).

| Directory | Contents |
| --- | --- |
| `backend/` | Qt-independent backend (camera control, recording, AI scheduling, person search) |
| `app/` | Qt desktop application `amdva-nvr` |
| `headless.cpp` | `amdva-streams`, which runs headless |
| `config/` | Example configuration files |
| `tools/`, `tests/`, `scripts/`, `benchmarks/` | Validation programs, tests, model preparation and benchmarks |

It is built together with the framework. If you do not use Qt, specify `-DAMDVA_BUILD_NVR_GUI=OFF` to build only `amdva-streams`. For settings, processing architecture and validation results, see the [NVR developer documentation](../../docs/dev/nvr/README.md).
