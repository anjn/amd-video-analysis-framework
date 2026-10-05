# Memory and element contracts

The public video path uses `video/x-raw(memory:VAMemory),format=NV12`. Width and
height must be even, progressive, and at most 32766. Declared color matrices
must be BT.709 and declared range must be limited; unknown colorimetry is treated
as BT.709 limited. Other memory types, HDR matrices, interlacing and P010 are
rejected. This first implementation does not negotiate arbitrary DMA-BUF input.

## Video, scaling and tensors

Both AMD elements are GstBaseTransform elements operating in place on the buffer
wrapper. They never map the original image. Metadata changes may create a new
buffer wrapper, but the original GstMemory and PTS remain the same.

`amdpreproc` borrows the upstream GstVaDisplay, holds a reference, and creates its
VideoProc context on the same VADisplay as the decoder. A separate VA display
would make upstream surface IDs invalid. VPE scales to the fitted model image
size and performs limited BT.709 NV12-to-BGRA conversion. Scaling must increment
the process's `drm-engine-vpe` counter; shader processing is not an accepted
fallback. This is intentionally a strict, driver-dependent capability check.

The internal output requests DRM linear modifier 0. Import accepts one DRM
object, checks plane offsets/pitches/size, and rejects tiled layouts, multi-object
surfaces, overlapping planes and out-of-bounds addressing. AMD's ROCm 7.2 Linux
external-memory path is validated with the alias probe; an arbitrary DMA-BUF is
not assumed interchangeable with a Vulkan Opaque FD. A successful import must
be validated for the exact runtime/driver combination. The caller's exported FD
is closed after mapping: the tested AMD runtime does not consume it. One internal
VA surface and its HIP mapping remain alive for the pipeline lifetime and are
reused after preprocessing completes. Repeatedly importing newly allocated
surfaces retained shared GPU allocations on the tested stack despite successful
API teardown. Fixed geometry and persistent import bound this retention; a change
in scaled dimensions requires pipeline restart. Mapping and imported-object
handles are destroyed during teardown.

HIP reads the imported BGRA allocation and writes the model input allocation
directly. It reorders channels, normalizes to `[0,1]`, and pads with RGB 114.
Scaling and conversion produce new computed outputs; no staging memcpy is
introduced. The VA output/import stays alive until the HIP completion event.

`AmdDeviceTensorMeta` holds a shared owner of the GPU input allocation, device,
shape, completion event and geometry. Full buffer metadata copies share this
owner; region copies and other transforms do not copy the tensor metadata.
Both plugins and clients link the same `amdva_gst_support` library, avoiding
duplicate metadata API registration. `amdmigraphx` waits for completion,
validates shape/device, binds device pointers, and removes tensor metadata after
inference. Allocations cannot be released or reused while a consumer owns them.

## Model and output contracts

The prepared model uses float32 external I/O and FP16 internal computation.
Runtime checks its SHA256/GPU manifest, static input shape, contiguous output
shape, device output pointer and the YOLOv8 model metadata. Additional inputs
are rejected; explicit compiled output parameters receive persistent device
allocations. Dynamic shapes, alternate model families and arbitrary postprocessors
are outside this PoC.

GPU postprocessing decodes class scores and performs class-aware greedy NMS,
with at most 300 detections. NMS selection is serial on the GPU in this PoC;
throughput tuning is a later optimization. CPU readback is bounded and consists
only of detection records. Coordinate restoration uses the actual rounded scale
dimensions and padding, clips to the original frame, and discards empty boxes.

GstAnalytics object detections link to an 80-class descriptor tagged
`class_descriptor`. The selected DL Streamer tracker is patched to obtain class
IDs from that descriptor, rather than requiring a legacy detection tensor.
The patch also narrows OpenCV includes to the modules actually used and makes
JSON classification strings borrowed references, avoiding a leak when a label
exists but the optional model name is absent. Imageless
tracking avoids image mapping; conversion and publication consume metadata.
All selected elements share one DL Streamer metadata library.

## Configuration and lifecycle

| Element | Properties | Default |
| --- | --- | --- |
| amdpreproc | device, tensor-width, tensor-height, require-zero-copy | 0, 640, 640, true |
| amdmigraphx | model, model-info, device, confidence-threshold, nms-threshold, require-zero-copy | required paths, 0, 0.25, 0.45, true |

Properties may change only in NULL/READY. Setting strict mode false is rejected
on initialization; no silent CPU/copy fallback exists. Initialization, import,
model, inference and metadata failures produce GStreamer bus errors.
Debug categories are `amdpreproc` and `amdmigraphx`. State teardown releases
resources. EOS drains synchronously; flush events follow GstBaseTransform behavior
and cannot cancel an already submitted HIP kernel. A VA display change requires
pipeline restart. Batching and inference frame sampling are not implemented.

The host-local prefix isolates userspace libraries. Kernel, firmware and Mesa
remain host dependencies, and are captured in the validation environment record.
