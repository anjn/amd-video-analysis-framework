#include "amdva/runtime.hpp"
#include <va/va_drm.h>
#include <va/va_vpp.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>
#include <limits>

namespace amdva {
void hip_check(hipError_t s, const char* op) {
    if (s != hipSuccess) throw std::runtime_error(std::string(op) + ": " + hipGetErrorString(s));
}
void va_check(VAStatus s, const char* op) {
    if (s != VA_STATUS_SUCCESS) throw std::runtime_error(std::string(op) + ": " + vaErrorStr(s));
}
Layout validate_layout(const VADRMPRIMESurfaceDescriptor& d) {
    if (!d.width || !d.height) throw std::runtime_error("Zero-sized surface");
    if (d.num_objects != 1 || d.objects[0].drm_format_modifier != 0)
        throw std::runtime_error("Strict import requires one linear DRM object; no detile/copy fallback");
    Layout l{d.width, d.height, d.fourcc, d.objects[0].size, 0, 0, 0, 0, 0};
    if(d.fourcc==VA_FOURCC_BGRA) {
        if(d.num_layers!=1 || d.layers[0].num_planes!=1 || d.layers[0].object_index[0])
            throw std::runtime_error("Invalid BGRA plane layout");
        l.y_offset=d.layers[0].offset[0]; l.y_pitch=d.layers[0].pitch[0]; l.pixel_stride=4;
        if(uint64_t(l.y_pitch)<uint64_t(l.width)*4 ||
           uint64_t(l.y_offset)+uint64_t(l.y_pitch)*(l.height-1)+uint64_t(l.width)*4>l.object_size)
            throw std::runtime_error("BGRA plane exceeds exported object bounds");
        return l;
    }
    if(d.fourcc!=VA_FOURCC_NV12 || d.width%2 || d.height%2)
        throw std::runtime_error("Supported imports: BGRA or even-sized NV12");
    if (d.num_layers == 1 && d.layers[0].num_planes == 2) {
        if (d.layers[0].object_index[0] || d.layers[0].object_index[1])
            throw std::runtime_error("Invalid plane object index");
        l.y_offset=d.layers[0].offset[0]; l.uv_offset=d.layers[0].offset[1];
        l.y_pitch=d.layers[0].pitch[0]; l.uv_pitch=d.layers[0].pitch[1];
    } else if (d.num_layers == 2 && d.layers[0].num_planes == 1 && d.layers[1].num_planes == 1) {
        if (d.layers[0].object_index[0] || d.layers[1].object_index[0])
            throw std::runtime_error("Invalid plane object index");
        l.y_offset=d.layers[0].offset[0]; l.uv_offset=d.layers[1].offset[0];
        l.y_pitch=d.layers[0].pitch[0]; l.uv_pitch=d.layers[1].pitch[0];
    } else throw std::runtime_error("Unsupported NV12 plane layout");
    const uint64_t y_end=uint64_t(l.y_offset)+uint64_t(l.y_pitch)*(l.height-1)+l.width;
    const uint64_t uv_end=uint64_t(l.uv_offset)+uint64_t(l.uv_pitch)*(l.height/2-1)+l.width;
    if (l.y_pitch < l.width || l.uv_pitch < l.width || y_end > l.object_size || uv_end > l.object_size)
        throw std::runtime_error("NV12 planes exceed exported object bounds");
    if (!(y_end <= l.uv_offset || uv_end <= l.y_offset))
        throw std::runtime_error("NV12 planes overlap");
    return l;
}
DeviceBuffer::DeviceBuffer(size_t b):bytes_(b) {
    if (!b) throw std::runtime_error("Zero-sized device buffer");
    hip_check(hipMalloc(&data_, b), "hipMalloc");
}
DeviceBuffer::~DeviceBuffer() { if (data_) (void)hipFree(data_); }
ImportedSurface::ImportedSurface(VADRMPRIMESurfaceDescriptor d) {
    auto close_all=[&] { for(unsigned i=0;i<std::min(d.num_objects,4u);++i) if(d.objects[i].fd>=0) close(d.objects[i].fd); };
    try {
        layout_=validate_layout(d);
        hipExternalMemoryHandleDesc h{};
        h.type=hipExternalMemoryHandleTypeOpaqueFd; h.handle.fd=d.objects[0].fd; h.size=layout_.object_size;
        // ROCm's Linux external-buffer path imports the DRM allocation. Compatibility
        // is experimental and must pass the alias probe; this is not a general FD guarantee.
        hip_check(hipImportExternalMemory(&memory_, &h), "HIP import of VA DRM object");
        // AMD's validated ROCm 7.2 Linux path imports the BO but retains the
        // caller's export FD. Close it after mapping, including on error.
        // Assuming CUDA-style FD consumption leaks one FD per video frame.
        hipExternalMemoryBufferDesc b{}; b.size=layout_.object_size;
        hip_check(hipExternalMemoryGetMappedBuffer(&pointer_, memory_, &b), "HIP map of VA DRM object");
        close_all();
    } catch (...) {
        if(pointer_) (void)hipFree(pointer_);
        if(memory_) (void)hipDestroyExternalMemory(memory_);
        close_all(); throw;
    }
}
ImportedSurface::~ImportedSurface() {
    if(pointer_) (void)hipFree(pointer_);
    if(memory_) (void)hipDestroyExternalMemory(memory_);
}
Kernel::Kernel(const char* source, const char* entry, const std::string& arch) {
    hiprtcProgram p{};
    auto check=[](hiprtcResult s) { if(s!=HIPRTC_SUCCESS) throw std::runtime_error(hiprtcGetErrorString(s)); };
    check(hiprtcCreateProgram(&p, source, "amdva-kernel.hip", 0, nullptr, nullptr));
    try {
        const std::string opt="--gpu-architecture="+arch;
        const char* opts[]={opt.c_str()};
        auto status=hiprtcCompileProgram(p,1,opts);
        if(status!=HIPRTC_SUCCESS) {
            size_t size{}; hiprtcGetProgramLogSize(p,&size); std::string log(size,'\0');
            if(size) hiprtcGetProgramLog(p,log.data());
            throw std::runtime_error("HIPRTC compilation: "+log);
        }
        size_t size{}; check(hiprtcGetCodeSize(p,&size)); std::vector<char> code(size);
        check(hiprtcGetCode(p,code.data()));
        hip_check(hipModuleLoadData(&module_,code.data()),"hipModuleLoadData");
        hip_check(hipModuleGetFunction(&function_,module_,entry),"hipModuleGetFunction");
        hiprtcDestroyProgram(&p);
    } catch (...) { hiprtcDestroyProgram(&p); if(module_) (void)hipModuleUnload(module_); throw; }
}
Kernel::~Kernel() { if(module_) (void)hipModuleUnload(module_); }
void Kernel::launch(unsigned n, void** args, hipStream_t s) const {
    hip_check(hipModuleLaunchKernel(function_,(n+255)/256,1,1,256,1,1,0,s,args,nullptr),"HIP kernel launch");
}
std::string gpu_arch(int device) {
    hip_check(hipSetDevice(device),"hipSetDevice");
    hipDeviceProp_t p{}; hip_check(hipGetDeviceProperties(&p,device),"hipGetDeviceProperties");
    return std::string(p.gcnArchName).substr(0,std::string(p.gcnArchName).find(':'));
}
VaSession::VaSession(const std::string& node) {
    fd_=open(node.c_str(),O_RDWR|O_CLOEXEC);
    if(fd_<0) throw std::runtime_error("Cannot open render node: "+node);
    bool initialized=false;
    try {
        display_=vaGetDisplayDRM(fd_); if(!display_) throw std::runtime_error("vaGetDisplayDRM failed");
        int major{},minor{}; va_check(vaInitialize(display_,&major,&minor),"vaInitialize"); initialized=true;
        vendor_=vaQueryVendorString(display_);
        va_check(vaCreateConfig(display_,VAProfileNone,VAEntrypointVideoProc,nullptr,0,&config_),"VA VideoProc config");
        va_check(vaCreateContext(display_,config_,0,0,0,nullptr,0,&context_),"VA VideoProc context");
    } catch (...) {
        if(config_!=VA_INVALID_ID) vaDestroyConfig(display_,config_);
        if(initialized) vaTerminate(display_);
        close(fd_); throw;
    }
}
VaSession::~VaSession() {
    if(context_!=VA_INVALID_ID) vaDestroyContext(display_,context_);
    if(config_!=VA_INVALID_ID) vaDestroyConfig(display_,config_);
    if(display_ && owned_) vaTerminate(display_);
    if(fd_>=0) close(fd_);
}
VaSession::VaSession(VADisplay display):display_(display),owned_(false) {
    if(!display_) throw std::runtime_error("Missing upstream VA display");
    vendor_=vaQueryVendorString(display_);
    va_check(vaCreateConfig(display_,VAProfileNone,VAEntrypointVideoProc,nullptr,0,&config_),"VA VideoProc config");
    try { va_check(vaCreateContext(display_,config_,0,0,0,nullptr,0,&context_),"VA VideoProc context"); }
    catch (...) { vaDestroyConfig(display_,config_); config_=VA_INVALID_ID; throw; }
}
VASurfaceID VaSession::create_nv12(unsigned w,unsigned h,bool linear) {
    VASurfaceAttrib attrs[3]{};
    attrs[0].type=VASurfaceAttribPixelFormat; attrs[0].flags=VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type=VAGenericValueTypeInteger; attrs[0].value.value.i=VA_FOURCC_NV12;
    attrs[1].type=VASurfaceAttribUsageHint; attrs[1].flags=VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type=VAGenericValueTypeInteger; attrs[1].value.value.i=VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE;
    uint64_t modifier=0; VADRMFormatModifierList modifiers{1,&modifier};
    attrs[2].type=VASurfaceAttribDRMFormatModifiers; attrs[2].flags=VA_SURFACE_ATTRIB_SETTABLE;
    attrs[2].value.type=VAGenericValueTypePointer; attrs[2].value.value.p=&modifiers;
    VASurfaceID s=VA_INVALID_SURFACE;
    va_check(vaCreateSurfaces(display_,VA_RT_FORMAT_YUV420,w,h,&s,1,attrs,linear?3:2),"VA NV12 allocation");
    return s;
}
void VaSession::destroy_surface(VASurfaceID s) noexcept { if(s!=VA_INVALID_SURFACE) vaDestroySurfaces(display_,&s,1); }
VASurfaceID VaSession::create_bgra(unsigned w,unsigned h) {
    uint64_t modifier=0; VADRMFormatModifierList modifiers{1,&modifier};
    VASurfaceAttrib attrs[3]{};
    attrs[0].type=VASurfaceAttribPixelFormat; attrs[0].flags=VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type=VAGenericValueTypeInteger; attrs[0].value.value.i=VA_FOURCC_BGRA;
    attrs[1].type=VASurfaceAttribUsageHint; attrs[1].flags=VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type=VAGenericValueTypeInteger; attrs[1].value.value.i=VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE;
    attrs[2].type=VASurfaceAttribDRMFormatModifiers; attrs[2].flags=VA_SURFACE_ATTRIB_SETTABLE;
    attrs[2].value.type=VAGenericValueTypePointer; attrs[2].value.value.p=&modifiers;
    VASurfaceID surface=VA_INVALID_SURFACE;
    va_check(vaCreateSurfaces(display_,VA_RT_FORMAT_RGB32,w,h,&surface,1,attrs,3),"VA linear BGRA allocation");
    return surface;
}
void VaSession::scale(VASurfaceID src,VASurfaceID dst,unsigned sw,unsigned sh,unsigned dw,unsigned dh) {
    if(std::max({sw,sh,dw,dh})>32767) throw std::runtime_error("VA rectangle dimensions exceed supported bounds");
    VARectangle in{0,0,static_cast<uint16_t>(sw),static_cast<uint16_t>(sh)};
    VARectangle out{0,0,static_cast<uint16_t>(dw),static_cast<uint16_t>(dh)};
    VAProcPipelineParameterBuffer p{}; p.surface=src; p.surface_region=&in; p.output_region=&out;
    p.surface_color_standard=VAProcColorStandardBT709; p.output_color_standard=VAProcColorStandardBT709;
    p.filter_flags=VA_FILTER_SCALING_DEFAULT;
    VABufferID b=VA_INVALID_ID;
    va_check(vaCreateBuffer(display_,context_,VAProcPipelineParameterBufferType,sizeof(p),1,&p,&b),"VA pipeline buffer");
    try {
        va_check(vaBeginPicture(display_,context_,dst),"VA begin scale");
        va_check(vaRenderPicture(display_,context_,&b,1),"VA render scale");
        va_check(vaEndPicture(display_,context_),"VA end scale");
        va_check(vaSyncSurface(display_,dst),"VA scale completion");
        vaDestroyBuffer(display_,b);
    } catch (...) { vaDestroyBuffer(display_,b); throw; }
}
VADRMPRIMESurfaceDescriptor VaSession::export_surface(VASurfaceID s) {
    va_check(vaSyncSurface(display_,s),"VA export readiness");
    VADRMPRIMESurfaceDescriptor d{};
    va_check(vaExportSurfaceHandle(display_,s,VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY|VA_EXPORT_SURFACE_SEPARATE_LAYERS,&d),"VA export DMA-BUF");
    return d;
}
bool VaSession::has_h264_decode() const {
    std::vector<VAEntrypoint> eps(vaMaxNumEntrypoints(display_)); int n{};
    if(vaQueryConfigEntrypoints(display_,VAProfileH264High,eps.data(),&n)!=VA_STATUS_SUCCESS) return false;
    return std::find(eps.begin(),eps.begin()+n,VAEntrypointVLD)!=eps.begin()+n;
}
uint64_t engine_time_ns(const std::string& engine) {
    uint64_t total=0; std::set<std::string> clients;
    for(const auto& file: std::filesystem::directory_iterator("/proc/self/fdinfo")) {
        std::ifstream in(file.path()); std::string line,id; uint64_t value=0;
        while(std::getline(in,line)) {
            if(line.rfind("drm-client-id:",0)==0) id=line.substr(14);
            if(line.rfind("drm-engine-"+engine+":",0)==0) { std::istringstream s(line.substr(line.find(':')+1)); s>>value; }
        }
        if(!id.empty() && clients.insert(id).second) total+=value;
    }
    return total;
}
} // namespace amdva
