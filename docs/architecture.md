# Memory and execution contracts

See [framework interfaces and examples](framework.md) for tasks, elements,
model preparation, batching and SDK usage. This document defines the GPU memory
contract independently of application UI and result transports.

## Video and preprocessing

Analytics elements accept `video/x-raw(memory:VAMemory),format=NV12`, or a
video-only GstAnalytics batch container whose children have those caps.
Dimensions must be even and progressive. Declared matrices must be BT.709 and
range limited; unknown colorimetry is treated as BT.709 limited. P010, HDR,
interlacing, system-memory inputs and arbitrary DMA-BUF decoder inputs are
rejected. The NVR's validated admission limit is 1920x1080; the standalone
watermark accepts up to 4096x4096. Inference caps advertise up to 32766 but larger
geometries remain subject to driver and intermediate-memory limits.

Inference transforms retain the original video GstMemory and PTS. They borrow
and retain the decoder's GstVaDisplay: surface IDs are only valid on that same
VA display. Separate container streams have separate preprocessors, including
separate retained displays. A display change in one stream requires recreating
its preprocessor; a new upstream stream-start identity triggers that reset.

VPE crops, scales and converts NV12 to a linear BGRA model surface. Every VA
operation must increment the process's `drm-engine-vpe` counter; shader or CPU
scaling is not a fallback. NV12 ROI boundaries expand outward to 2x2 chroma
boundaries, and inverse geometry records the expanded rectangle. The shared
preprocessor explicitly decomposes ratios larger than 4x into VPE stages to
avoid the validated Mesa driver's failures on offset/asymmetric geometric
scaling. At most two intermediate VA surfaces are reused. Intermediate images
are limited to 64M pixels each. Geometry changes renew VideoProc contexts to
avoid stale driver coefficient caches; model output surfaces remain allocated.

The internal BGRA output requests DRM linear modifier 0. Import accepts one
DRM object and validates offsets, pitches, sizes and plane overlap. Tiled,
multi-object and out-of-bounds layouts are rejected. AMD's ROCm external-memory
path must be validated for the runtime/driver combination; arbitrary DMA-BUF
handles are not assumed equivalent to Vulkan Opaque FDs. The caller closes
exported FDs after mapping because the validated AMD runtime does not consume
them. Imported-object handles and mappings are destroyed on teardown.

HIP reads the persistent imported BGRA image and writes a new float32 NCHW GPU
allocation with RGB/BGR order, scale/mean/std normalization and optional
letterbox padding. There is no staging image memcpy. The VA surface and import
remain alive through the HIP completion event. Each submitted tensor owns its
storage independently, including every ROI; later preprocessing cannot
overwrite a queued batch input.

## Models, batches and results

Prepared models have SHA256, GPU architecture and `offload_copy=false` manifests.
Schema 1 YOLO artifacts remain readable; the general compiler emits schema 2.
Named input tensors are checked for static shape, dtype, byte size, contiguous
layout and GPU device location. Output bindings are pooled only when no result
still owns them; otherwise a new GPU allocation is used. Models without usable
external output bindings use a device-to-device ownership copy. Outputs remain
valid after the next inference and after executor teardown.

`SharedBatchExecutor` bounds its FIFO (default 64 requests), admits one request
per source per batch, expires partial-batch waits and assembles inputs with
GPU-to-GPU copies. Unused batch entries are normalized padding generated on the
GPU. An explicit cancellation generation fails queued futures and suppresses
results from already-running jobs. Final source retirement releases cancellation
bookkeeping after in-flight work finishes. A completed GPU kernel cannot be
preempted by a GStreamer flush.

GPU postprocessing includes class-aware greedy NMS (up to 300 objects),
classification softmax/top-k (up to 100 classes), semantic argmax, instance-mask
prototype reconstruction and pose keypoint extraction. Compact detection,
classification and keypoint records are the only result readbacks. Masks and
raw output tensors stay on the GPU. Large tensors and mask allocations have
explicit shape/budget checks. The serial GPU NMS implementation prioritizes
correctness over maximum detector throughput.

Inverse image transforms use rounded scale dimensions, crop and padding,
clip boxes to the source and discard empty boxes. Standard GstAnalytics
relations carry object/classification/tracking/keypoint metadata.
`AmdAnalysisMeta` carries source identity and GPU attachments. Full metadata
copies clone mutable lists and share GPU storage owners; region transforms are
rejected. The selected `gvametaaggregate` patch remaps parent metadata IDs and
rescales attachment geometry while retaining GPU ownership. It rejects merges
with incompatible source/generation/PTS. Multiple distinct custom JSON values
are retained under `custom.branches`.

## Drawing and output

`amdwatermark` produces BGRA DMA-BUF output using VPE and HIP. Its four-slot
pool retains VA display, surface, HIP import and exported FD until every copy of
the output GstMemory is released. Holding all four frames introduces bounded
backpressure; flush wakes the waiting producer. Output metadata and PTS survive,
while video layout metadata is replaced with the BGRA offsets and pitch.
Boxes, ASCII class labels/IDs, masks and COCO-17 skeletons are generated on the
GPU. Drawing commands are bounded to 8192; text supports Latin letters, digits
and `#`. The NVR EGL renderer uses the shared overlay geometry for its tile
boxes and labels without mapping image memory.

The metadata-only Python hook runs under the GIL on the streaming thread.
JSON serialization does not read images or masks. The asynchronous publisher
bounds records/bytes, drops oldest queued records when full, retries destinations
and counts delivery failures. Broker work does not hold video buffers or GPU
owners. Applications requiring durable delivery must add their own event store.

## Copy auditing and lifecycle

The audit preload rejects decoded-image CPU maps and unmarked host/device
transfers. It permits device-to-device batch/ownership copies, the legacy
7204-byte detection readback, and explicitly marked metadata transfers up to
256 KiB. Uploads contain bounded drawing commands; readbacks contain compact
results. This is API-level auditing, not proof about every transfer internal to
the driver. Compressed RTSP packet parsing/recording and JSON are CPU operations;
they do not expose decoded pixels.

Model preparation and offline reference tools intentionally transfer tensors
or images for comparison and are outside the runtime audit contract. Initialize
models/kernels before starting steady-state auditing when measuring runtime
copies. Driver initialization and model constant loading are not per-frame
pixel processing.

All element configuration properties are NULL/READY mutable. Strict mode cannot
be disabled. Initialization, inference, unsupported geometry and GPU failures
raise bus errors; there is no silent CPU fallback. Properties and caps are
validated before processing. EOS drains synchronous inference. Flush retires
old generations, and shared owners keep submitted inputs/outputs alive through
completion. The rootless userspace prefix does not replace the host kernel,
firmware, Mesa or device permissions.
