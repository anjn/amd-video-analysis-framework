#include "amdva/runtime.hpp"
#include <gst/gst.h>
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <mutex>
#include <set>
namespace {
std::atomic<bool> active{false};
std::atomic<unsigned long long> violations{0},copies{0},bytes{0},layout_queries{0};
thread_local const void* metadata_pointer{};
thread_local size_t metadata_bytes{};
thread_local int metadata_kind{};
struct Surfaces {std::mutex mutex;std::set<std::pair<VADisplay,VASurfaceID>> unused;};
Surfaces& surfaces(){static Surfaces value;return value;}
template<class F> F original(const char* name) {return reinterpret_cast<F>(dlsym(RTLD_NEXT,name));}
void copy(size_t size,hipMemcpyKind kind,const void* source) {
    if(!active.load())return;
    ++copies;bytes+=size;
    const bool metadata=source==metadata_pointer&&size==metadata_bytes&&int(kind)==metadata_kind;
    // GPU batch assembly stays on-device. Host transfers require an exact,
    // bounded metadata declaration (or the legacy detection ABI size).
    if(kind!=hipMemcpyDeviceToDevice&&!metadata&&(kind!=hipMemcpyDeviceToHost||size!=7204)) {
        ++violations;std::fprintf(stderr,"AMDVA_AUDIT prohibited copy: bytes=%zu kind=%d\n",size,int(kind));
    }
}
void image_operation(const char* operation) {
    if(active.load()) {++violations;std::fprintf(stderr,"AMDVA_AUDIT prohibited image operation: %s\n",operation);}
}
}
extern "C" void amdva_audit_start() {active=true;}
extern "C" void amdva_audit_metadata_begin(const void* pointer,size_t size,int kind){
    if(size<=256*1024){metadata_pointer=pointer;metadata_bytes=size;metadata_kind=kind;}
}
extern "C" void amdva_audit_metadata_end(){metadata_pointer=nullptr;metadata_bytes=0;}
extern "C" unsigned long long amdva_audit_errors() {return violations.load();}
extern "C" unsigned long long amdva_audit_copies() {return copies.load();}
extern "C" unsigned long long amdva_audit_bytes() {return bytes.load();}
extern "C" unsigned long long amdva_audit_layout_queries() {return layout_queries.load();}
extern "C" void amdva_audit_stop() {active=false;}
extern "C" hipError_t hipMemcpy(void* dst,const void* src,size_t size,hipMemcpyKind kind) {
    copy(size,kind,src);auto f=original<decltype(&hipMemcpy)>("hipMemcpy");return f?f(dst,src,size,kind):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyAsync(void* dst,const void* src,size_t size,hipMemcpyKind kind,hipStream_t stream) {
    copy(size,kind,src);auto f=original<decltype(&hipMemcpyAsync)>("hipMemcpyAsync");return f?f(dst,src,size,kind,stream):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyDtoD(hipDeviceptr_t dst,hipDeviceptr_t src,size_t size) {
    copy(size,hipMemcpyDeviceToDevice,src);auto f=original<decltype(&hipMemcpyDtoD)>("hipMemcpyDtoD");return f?f(dst,src,size):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyDtoDAsync(hipDeviceptr_t dst,hipDeviceptr_t src,size_t size,hipStream_t stream) {
    copy(size,hipMemcpyDeviceToDevice,src);auto f=original<decltype(&hipMemcpyDtoDAsync)>("hipMemcpyDtoDAsync");return f?f(dst,src,size,stream):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyHtoD(hipDeviceptr_t dst,const void* src,size_t size) {
    copy(size,hipMemcpyHostToDevice,src);auto f=original<decltype(&hipMemcpyHtoD)>("hipMemcpyHtoD");return f?f(dst,src,size):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyDtoH(void* dst,hipDeviceptr_t src,size_t size) {
    copy(size,hipMemcpyDeviceToHost,src);auto f=original<decltype(&hipMemcpyDtoH)>("hipMemcpyDtoH");return f?f(dst,src,size):hipErrorNotSupported;
}
extern "C" gboolean gst_memory_map(GstMemory* memory,GstMapInfo* info,GstMapFlags flags) {
    if(memory && memory->allocator && memory->allocator->mem_type &&
       std::string(memory->allocator->mem_type)=="VAMemory") image_operation("gst_memory_map(VAMemory)");
    auto f=original<decltype(&gst_memory_map)>("gst_memory_map");return f?f(memory,info,flags):FALSE;
}
extern "C" VAStatus vaDeriveImage(VADisplay d,VASurfaceID s,VAImage* i) {
    // GStreamer's allocator derives a newly-created, never-rendered test
    // surface only to discover pitches. It destroys the image without mapping
    // or copying pixels. Live decoded/VPE surfaces are never exempted.
    bool layout=false;{auto& registry=surfaces();std::lock_guard<std::mutex> lock(registry.mutex);layout=registry.unused.count({d,s})!=0;}
    if(layout){if(active.load())++layout_queries;}else image_operation("vaDeriveImage");auto f=original<decltype(&vaDeriveImage)>("vaDeriveImage");return f?f(d,s,i):VA_STATUS_ERROR_OPERATION_FAILED;
}
extern "C" VAStatus vaGetImage(VADisplay d,VASurfaceID s,int x,int y,unsigned w,unsigned h,VAImageID i) {
    image_operation("vaGetImage");auto f=original<decltype(&vaGetImage)>("vaGetImage");return f?f(d,s,x,y,w,h,i):VA_STATUS_ERROR_OPERATION_FAILED;
}
extern "C" VAStatus vaPutImage(VADisplay d,VASurfaceID s,VAImageID i,int sx,int sy,unsigned sw,unsigned sh,int dx,int dy,unsigned dw,unsigned dh) {
    image_operation("vaPutImage");auto f=original<decltype(&vaPutImage)>("vaPutImage");return f?f(d,s,i,sx,sy,sw,sh,dx,dy,dw,dh):VA_STATUS_ERROR_OPERATION_FAILED;
}

extern "C" VAStatus vaCreateSurfaces(VADisplay d,unsigned format,unsigned w,unsigned h,VASurfaceID* ids,unsigned count,VASurfaceAttrib* attrs,unsigned attr_count) {
    auto f=original<decltype(&vaCreateSurfaces)>("vaCreateSurfaces");if(!f)return VA_STATUS_ERROR_OPERATION_FAILED;
    const auto result=f(d,format,w,h,ids,count,attrs,attr_count);
    if(result==VA_STATUS_SUCCESS){auto& registry=surfaces();std::lock_guard<std::mutex> lock(registry.mutex);for(unsigned i=0;i<count;++i)registry.unused.insert({d,ids[i]});}
    return result;
}
extern "C" VAStatus vaBeginPicture(VADisplay d,VAContextID context,VASurfaceID target) {
    {auto& registry=surfaces();std::lock_guard<std::mutex> lock(registry.mutex);registry.unused.erase({d,target});}
    auto f=original<decltype(&vaBeginPicture)>("vaBeginPicture");return f?f(d,context,target):VA_STATUS_ERROR_OPERATION_FAILED;
}
extern "C" VAStatus vaDestroySurfaces(VADisplay d,VASurfaceID* ids,int count) {
    {auto& registry=surfaces();std::lock_guard<std::mutex> lock(registry.mutex);for(int i=0;i<count;++i)registry.unused.erase({d,ids[i]});}
    auto f=original<decltype(&vaDestroySurfaces)>("vaDestroySurfaces");return f?f(d,ids,count):VA_STATUS_ERROR_OPERATION_FAILED;
}

// Opt-in GL image auditing, so existing diagnostic framebuffer captures remain
// explicit test operations. Enable for production dashboard streaming only.
namespace { std::atomic<bool> audit_gl_images{false}; }
extern "C" void amdva_audit_gl_images(bool enabled) {audit_gl_images=enabled;}
extern "C" void glReadPixels(int x,int y,int width,int height,unsigned format,unsigned type,void* pixels) {
    if(audit_gl_images)image_operation("glReadPixels");
    using F=void(*)(int,int,int,int,unsigned,unsigned,void*);
    auto f=original<F>("glReadPixels");if(f)f(x,y,width,height,format,type,pixels);
}
extern "C" void* glMapBufferRange(unsigned target,intptr_t offset,intptr_t length,unsigned access) {
    if(audit_gl_images&&target==0x88eb&&(access&1))image_operation("glMapBufferRange(pixel pack)");
    using F=void*(*)(unsigned,intptr_t,intptr_t,unsigned);
    auto f=original<F>("glMapBufferRange");return f?f(target,offset,length,access):nullptr;
}
extern "C" void glTexSubImage2D(unsigned target,int level,int x,int y,int width,int height,unsigned format,unsigned type,const void* pixels) {
    if(audit_gl_images&&pixels)image_operation("glTexSubImage2D(CPU upload)");
    using F=void(*)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
    auto f=original<F>("glTexSubImage2D");if(f)f(target,level,x,y,width,height,format,type,pixels);
}
extern "C" void glTexImage2D(unsigned target,int level,int internal,int width,int height,int border,unsigned format,unsigned type,const void* pixels) {
    if(audit_gl_images&&pixels)image_operation("glTexImage2D(CPU upload)");
    using F=void(*)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*);
    auto f=original<F>("glTexImage2D");if(f)f(target,level,internal,width,height,border,format,type,pixels);
}
using AuditGLProc=void(*)();
extern "C" AuditGLProc eglGetProcAddress(const char* name) {
    if(audit_gl_images&&name){
        const std::string s(name);
        if(s=="glReadPixels")return reinterpret_cast<AuditGLProc>(&glReadPixels);
        if(s=="glMapBufferRange")return reinterpret_cast<AuditGLProc>(&glMapBufferRange);
        if(s=="glTexImage2D")return reinterpret_cast<AuditGLProc>(&glTexImage2D);
        if(s=="glTexSubImage2D")return reinterpret_cast<AuditGLProc>(&glTexSubImage2D);
    }
    using F=AuditGLProc(*)(const char*);auto f=original<F>("eglGetProcAddress");return f?f(name):nullptr;
}
