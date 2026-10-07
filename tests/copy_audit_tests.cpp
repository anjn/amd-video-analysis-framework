#include "amdva/runtime.hpp"
#include <dlfcn.h>
#include <iostream>
int main() {
    try {
        amdva::VaSession va("/dev/dri/renderD128");
        const auto source = va.create_nv12(640, 360, true), target = va.create_bgra(640, 360);
        amdva::DeviceBuffer compact(7204);
        auto start = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        auto stop = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_stop"));
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        auto layouts = reinterpret_cast<unsigned long long (*)()>(
            dlsym(RTLD_DEFAULT, "amdva_audit_layout_queries"));
        if (!start || !stop || !errors || !layouts)
            throw std::runtime_error("Load copy auditor with LD_PRELOAD");
        start();
        VAImage image{};
        if (vaDeriveImage(va.display(), source, &image) == VA_STATUS_SUCCESS)
            vaDestroyImage(va.display(), image.image_id);
        if (errors() != 0 || layouts() != 1)
            throw std::runtime_error("Blank allocator probe incorrectly rejected");
        va.scale(source, target, 640, 360, 640, 360);
        if (vaDeriveImage(va.display(), target, &image) == VA_STATUS_SUCCESS)
            vaDestroyImage(va.display(), image.image_id);
        if (errors() != 1)
            throw std::runtime_error("Rendered image derivation not rejected");
        char bytes[7204]{};
        amdva::hip_check(hipMemcpy(bytes, compact.data(), sizeof(bytes), hipMemcpyDeviceToHost),
                         "Allowed detection readback");
        if (errors() != 1)
            throw std::runtime_error("Compact readback rejected");
        amdva::hip_check(hipMemcpy(bytes, compact.data(), 1, hipMemcpyDeviceToHost),
                         "Prohibited test copy");
        if (errors() != 2)
            throw std::runtime_error("Non-detection readback not rejected");
        amdva::upload_metadata(compact.data(),bytes,128);
        amdva::readback_metadata(bytes,compact.data(),128);
        amdva::DeviceBuffer second(7204);
        amdva::hip_check(hipMemcpy(second.data(),compact.data(),7204,hipMemcpyDeviceToDevice),"GPU batch assembly");
        if(errors()!=2)throw std::runtime_error("Marked metadata or GPU assembly incorrectly rejected");
        amdva::hip_check(hipMemcpy(compact.data(),bytes,128,hipMemcpyHostToDevice),"Prohibited upload");
        if(errors()!=3)throw std::runtime_error("Unmarked CPU upload not rejected");
        stop();
        va.destroy_surface(source);
        va.destroy_surface(target);
        std::cout
            << "Blank layout probes, live-image rejection and exact compact transfer size passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
