#include "amdva/runtime.hpp"
#include <unistd.h>
#include <iostream>
int main() {
    try {
        amdva::gpu_arch(0);amdva::VaSession va("/dev/dri/renderD128");
        auto surface=va.create_bgra(640,360);
        for(int i=0;i<10000;i++) {
            auto d=va.export_surface(surface);auto l=amdva::validate_layout(d);
            hipExternalMemoryHandleDesc h{};h.type=hipExternalMemoryHandleTypeOpaqueFd;h.handle.fd=d.objects[0].fd;h.size=l.object_size;
            hipExternalMemory_t memory{};amdva::hip_check(hipImportExternalMemory(&memory,&h),"stress import");
            hipExternalMemoryBufferDesc b{};b.size=l.object_size;void* ptr{};
            amdva::hip_check(hipExternalMemoryGetMappedBuffer(&ptr,memory,&b),"stress mapping");
            close(d.objects[0].fd);
            amdva::hip_check(hipFree(ptr),"stress hipFree external view");
            amdva::hip_check(hipDestroyExternalMemory(memory),"stress import destruction");
        }
        va.destroy_surface(surface);std::cout<<"10000 import/map/free cycles passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 2;}
}
