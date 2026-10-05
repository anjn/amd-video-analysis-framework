#include "amdva/runtime.hpp"
#include <gst/gst.h>
#include <atomic>
#include <cstdio>
#include <dlfcn.h>
namespace {
std::atomic<bool> active{false};
std::atomic<unsigned long long> violations{0},copies{0},bytes{0};
template<class F> F original(const char* name) {return reinterpret_cast<F>(dlsym(RTLD_NEXT,name));}
void copy(size_t size,hipMemcpyKind kind) {
    if(!active.load())return;
    ++copies;bytes+=size;
    // The only transfer in the analytics path is the 7204-byte compact
    // detection structure; no H2D/D2D or other sized transfers are allowed.
    if(kind!=hipMemcpyDeviceToHost || size!=7204) {
        ++violations;std::fprintf(stderr,"AMDVA_AUDIT prohibited copy: bytes=%zu kind=%d\n",size,int(kind));
    }
}
void image_operation(const char* operation) {
    if(active.load()) {++violations;std::fprintf(stderr,"AMDVA_AUDIT prohibited image operation: %s\n",operation);}
}
}
extern "C" void amdva_audit_start() {active=true;}
extern "C" unsigned long long amdva_audit_errors() {return violations.load();}
extern "C" unsigned long long amdva_audit_copies() {return copies.load();}
extern "C" unsigned long long amdva_audit_bytes() {return bytes.load();}
extern "C" void amdva_audit_stop() {active=false;}
extern "C" hipError_t hipMemcpy(void* dst,const void* src,size_t size,hipMemcpyKind kind) {
    copy(size,kind);auto f=original<decltype(&hipMemcpy)>("hipMemcpy");return f?f(dst,src,size,kind):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyAsync(void* dst,const void* src,size_t size,hipMemcpyKind kind,hipStream_t stream) {
    copy(size,kind);auto f=original<decltype(&hipMemcpyAsync)>("hipMemcpyAsync");return f?f(dst,src,size,kind,stream):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyDtoD(hipDeviceptr_t dst,hipDeviceptr_t src,size_t size) {
    copy(size,hipMemcpyDeviceToDevice);auto f=original<decltype(&hipMemcpyDtoD)>("hipMemcpyDtoD");return f?f(dst,src,size):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyDtoDAsync(hipDeviceptr_t dst,hipDeviceptr_t src,size_t size,hipStream_t stream) {
    copy(size,hipMemcpyDeviceToDevice);auto f=original<decltype(&hipMemcpyDtoDAsync)>("hipMemcpyDtoDAsync");return f?f(dst,src,size,stream):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyHtoD(hipDeviceptr_t dst,const void* src,size_t size) {
    copy(size,hipMemcpyHostToDevice);auto f=original<decltype(&hipMemcpyHtoD)>("hipMemcpyHtoD");return f?f(dst,src,size):hipErrorNotSupported;
}
extern "C" hipError_t hipMemcpyDtoH(void* dst,hipDeviceptr_t src,size_t size) {
    copy(size,hipMemcpyDeviceToHost);auto f=original<decltype(&hipMemcpyDtoH)>("hipMemcpyDtoH");return f?f(dst,src,size):hipErrorNotSupported;
}
extern "C" gboolean gst_memory_map(GstMemory* memory,GstMapInfo* info,GstMapFlags flags) {
    if(memory && memory->allocator && memory->allocator->mem_type &&
       std::string(memory->allocator->mem_type)=="VAMemory") image_operation("gst_memory_map(VAMemory)");
    auto f=original<decltype(&gst_memory_map)>("gst_memory_map");return f?f(memory,info,flags):FALSE;
}
extern "C" VAStatus vaDeriveImage(VADisplay d,VASurfaceID s,VAImage* i) {
    image_operation("vaDeriveImage");auto f=original<decltype(&vaDeriveImage)>("vaDeriveImage");return f?f(d,s,i):VA_STATUS_ERROR_OPERATION_FAILED;
}
extern "C" VAStatus vaGetImage(VADisplay d,VASurfaceID s,int x,int y,unsigned w,unsigned h,VAImageID i) {
    image_operation("vaGetImage");auto f=original<decltype(&vaGetImage)>("vaGetImage");return f?f(d,s,x,y,w,h,i):VA_STATUS_ERROR_OPERATION_FAILED;
}
extern "C" VAStatus vaPutImage(VADisplay d,VASurfaceID s,VAImageID i,int sx,int sy,unsigned sw,unsigned sh,int dx,int dy,unsigned dw,unsigned dh) {
    image_operation("vaPutImage");auto f=original<decltype(&vaPutImage)>("vaPutImage");return f?f(d,s,i,sx,sy,sw,sh,dx,dy,dw,dh):VA_STATUS_ERROR_OPERATION_FAILED;
}
