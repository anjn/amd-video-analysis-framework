#pragma once
#include <hip/hip_runtime_api.h>
#include <hip/hiprtc.h>
// HIP host headers define this token as a macro; GLib uses it as an
// attribute identifier in __has_attribute. Device code is compiled separately.
#ifdef __noinline__
#undef __noinline__
#endif
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace amdva {
void hip_check(hipError_t status, const char* operation);
void va_check(VAStatus status, const char* operation);
// Explicit bounded control/analysis transfers. Images and raw tensors must not
// use these helpers. The copy auditor checks the exact pointer, size and kind.
void readback_metadata(void* host, const void* device, size_t bytes);
void upload_metadata(void* device, const void* host, size_t bytes);
struct Layout {
    uint32_t width{}, height{}, fourcc{};
    uint32_t object_size{}, y_offset{}, uv_offset{}, y_pitch{}, uv_pitch{};
    uint64_t modifier{};
    uint32_t pixel_stride{1};
};
Layout validate_layout(const VADRMPRIMESurfaceDescriptor& desc);

class DeviceBuffer {
public:
    explicit DeviceBuffer(size_t bytes);
    // Non-owning view for synchronous APIs. The caller retains GPU ownership.
    DeviceBuffer(void* borrowed, size_t bytes);
    ~DeviceBuffer();
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    void* data() const { return data_; }
    size_t size() const { return bytes_; }
private:
    void* data_{};
    size_t bytes_{};
    bool owned_{true};
};

class ImportedSurface {
public:
    // Takes ownership of every FD in the export descriptor, including on failure.
    explicit ImportedSurface(VADRMPRIMESurfaceDescriptor desc);
    ~ImportedSurface();
    ImportedSurface(const ImportedSurface&) = delete;
    ImportedSurface& operator=(const ImportedSurface&) = delete;
    const Layout& layout() const { return layout_; }
    const uint8_t* data() const { return static_cast<const uint8_t*>(pointer_); }
private:
    Layout layout_{};
    hipExternalMemory_t memory_{};
    void* pointer_{};
};

class Kernel {
public:
    Kernel(const char* source, const char* entry, const std::string& arch);
    ~Kernel();
    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;
    void launch(unsigned count, void** args, hipStream_t stream = nullptr) const;
private:
    hipModule_t module_{};
    hipFunction_t function_{};
};

class VaSession {
public:
    explicit VaSession(const std::string& render_node);
    explicit VaSession(VADisplay borrowed_display);
    ~VaSession();
    VaSession(const VaSession&) = delete;
    VaSession& operator=(const VaSession&) = delete;
    VADisplay display() const { return display_; }
    const std::string& vendor() const { return vendor_; }
    VASurfaceID create_nv12(unsigned width, unsigned height, bool linear);
    VASurfaceID create_bgra(unsigned width, unsigned height);
    void scale(VASurfaceID src, VASurfaceID dst, unsigned src_width,
               unsigned src_height, unsigned dst_width, unsigned dst_height);
    void crop_scale(VASurfaceID src, VASurfaceID dst, unsigned x, unsigned y,
                    unsigned width, unsigned height, unsigned dst_width, unsigned dst_height);
    VADRMPRIMESurfaceDescriptor export_surface(VASurfaceID surface, bool writable=false);
    void destroy_surface(VASurfaceID surface) noexcept;
    bool has_h264_decode() const;
private:
    int fd_{-1};
    VADisplay display_{};
    VAConfigID config_{VA_INVALID_ID};
    VAContextID context_{VA_INVALID_ID};
    std::string vendor_;
    bool owned_{true};
};
// Aggregate counters per DRM client, avoiding duplicate FD aliases.
uint64_t engine_time_ns(const std::string& engine);
std::string gpu_arch(int device);
} // namespace amdva
