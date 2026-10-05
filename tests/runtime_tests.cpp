#include "amdva/runtime.hpp"
#include <iostream>
#include <functional>
static void require(bool value) { if(!value) throw std::runtime_error("Test assertion failed"); }
static void rejected(const std::function<void()>& f) {
    bool failed=false; try { f(); } catch(const std::runtime_error&) { failed=true; } require(failed);
}
int main() {
    VADRMPRIMESurfaceDescriptor d{}; d.fourcc=VA_FOURCC_NV12; d.width=640; d.height=360;
    d.num_objects=1; d.objects[0].size=640*540; d.num_layers=2;
    for(int i=0;i<2;++i) { d.layers[i].num_planes=1; d.layers[i].pitch[0]=640; }
    d.layers[1].offset[0]=640*360;
    require(amdva::validate_layout(d).uv_offset==640*360);
    auto bad=d; bad.objects[0].drm_format_modifier=1; rejected([&]{amdva::validate_layout(bad);});
    bad=d; bad.objects[0].size--; rejected([&]{amdva::validate_layout(bad);});
    bad=d; bad.layers[1].offset[0]=0; rejected([&]{amdva::validate_layout(bad);});
    bad=d; bad.layers[0].pitch[0]=639; rejected([&]{amdva::validate_layout(bad);});
    bad=d; bad.layers[1].object_index[0]=1; rejected([&]{amdva::validate_layout(bad);});
    bad=d; bad.height=359; rejected([&]{amdva::validate_layout(bad);});
    std::cout<<"NV12 bounds, overlap, pitch, modifier and object validation passed\n";
}
