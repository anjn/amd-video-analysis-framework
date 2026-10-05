#include "amdva/runtime.hpp"
#include <cstring>
#include <iostream>
#include <iomanip>

static constexpr const char* source=R"(
extern "C" __global__ void sample(const unsigned char* base, unsigned offset,
 unsigned pitch, unsigned width, unsigned height, unsigned stride, unsigned long long* total) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
 if(i<width*height) atomicAdd(total,(unsigned long long)base[offset+(i/width)*pitch+(i%width)*stride]);
})";

// CPU uploads here are test fixtures, never part of the video pipeline.
static void fill(VADisplay display,VASurfaceID surface,unsigned w,unsigned h,unsigned char y) {
    VAImageFormat format{}; format.fourcc=VA_FOURCC_NV12; format.byte_order=VA_LSB_FIRST;
    VAImage image{};
    amdva::va_check(vaCreateImage(display,&format,w,h,&image),"probe image allocation");
    void* data{};
    try {
        amdva::va_check(vaMapBuffer(display,image.buf,&data),"probe fixture map");
        for(unsigned row=0;row<h;++row) std::memset(static_cast<char*>(data)+image.offsets[0]+row*image.pitches[0],y,w);
        for(unsigned row=0;row<h/2;++row) std::memset(static_cast<char*>(data)+image.offsets[1]+row*image.pitches[1],128,w);
        amdva::va_check(vaUnmapBuffer(display,image.buf),"probe fixture unmap"); data=nullptr;
        amdva::va_check(vaPutImage(display,surface,image.image_id,0,0,w,h,0,0,w,h),"probe fixture upload");
        vaDestroyImage(display,image.image_id);
    } catch (...) { if(data) vaUnmapBuffer(display,image.buf); vaDestroyImage(display,image.image_id); throw; }
}
int main(int argc,char** argv) {
    const std::string node=argc>1?argv[1]:"/dev/dri/renderD128";
    try {
        const auto arch=amdva::gpu_arch(0);
        amdva::VaSession va(node);
        std::cerr<<"VA driver: "<<va.vendor()<<"; HIP: "<<arch<<"\n";
        const auto src=va.create_nv12(1920,1080,false);
        const auto dst=va.create_bgra(640,360);
        try {
            amdva::Kernel sample(source,"sample",arch);
            amdva::DeviceBuffer result(sizeof(unsigned long long));
            fill(va.display(),src,1920,1080,64);
            const auto before=amdva::engine_time_ns("vpe");
            va.scale(src,dst,1920,1080,640,360);
            auto desc=va.export_surface(dst);
            std::cerr<<"Export: "<<desc.num_objects<<" object(s), modifier=0x"<<std::hex<<desc.objects[0].drm_format_modifier<<std::dec<<"\n";
            amdva::ImportedSurface imported(desc);
            const auto& l=imported.layout();
            const uint8_t* pointer=imported.data(); void* total=result.data();
            auto checksum=[&] {
                amdva::hip_check(hipMemset(total,0,sizeof(unsigned long long)),"probe counter reset");
                unsigned offset=l.y_offset,pitch=l.y_pitch,w=l.width,h=l.height,stride=l.pixel_stride;
                void* args[]={&pointer,&offset,&pitch,&w,&h,&stride,&total};
                sample.launch(w*h,args);
                unsigned long long sum{};
                amdva::hip_check(hipMemcpy(&sum,total,sizeof(sum),hipMemcpyDeviceToHost),"probe scalar readback");
                return sum;
            };
            const auto first=checksum();
            fill(va.display(),src,1920,1080,192);
            va.scale(src,dst,1920,1080,640,360);
            const auto second=checksum();
            const auto after=amdva::engine_time_ns("vpe");
            const uint64_t pixels=640*360;
            const bool alias=first>=55*pixels && first<=57*pixels && second>=204*pixels && second<=206*pixels;
            const bool vpe=after>before;
            std::cout<<"{\"schema\":1,\"gpu\":\""<<arch<<"\",\"h264_capability\":"<<(va.has_h264_decode()?"true":"false")
                <<",\"linear_bgra\":true,\"alias_observed\":"<<(alias?"true":"false")
                <<",\"first_checksum\":"<<first<<",\"second_checksum\":"<<second
                <<",\"vpe_ns_delta\":"<<(after>=before?after-before:0)
                <<",\"vpe_observed\":"<<(vpe?"true":"false")
                <<",\"pipeline_zero_copy_verified\":false,\"gate_passed\":"<<(alias&&vpe?"true":"false")<<"}\n";
            va.destroy_surface(dst); va.destroy_surface(src);
            // Import aliasing is a prerequisite, not proof about a complete pipeline.
            return alias&&vpe?0:2;
        } catch (...) { va.destroy_surface(dst); va.destroy_surface(src); throw; }
    } catch(const std::exception& e) {
        std::cerr<<"Probe rejected: "<<e.what()<<"\n";
        std::cout<<"{\"schema\":1,\"gate_passed\":false,\"pipeline_zero_copy_verified\":false}\n";
        return 2;
    }
}
