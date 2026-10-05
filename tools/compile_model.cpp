#include "amdva/runtime.hpp"
#include <migraphx/migraphx.hpp>
#include <glib.h>
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=3) {std::cerr<<"Usage: amdva-compile-model MODEL.onnx OUTPUT.mxr\n";return 1;}
    try {
        const auto arch=amdva::gpu_arch(0);
        migraphx::onnx_options options;options.set_default_dim_value(1);
        auto program=migraphx::parse_onnx(argv[1],options);
        migraphx::quantize_fp16(program);
        migraphx::compile_options compile;compile.set_offload_copy(false);
        program.compile(migraphx::target("gpu"),compile);
        migraphx::save(program,argv[2]);
        gchar* data{};gsize size{};
        if(!g_file_get_contents(argv[2],&data,&size,nullptr)) throw std::runtime_error("Cannot hash compiled model");
        gchar* hash=g_compute_checksum_for_data(G_CHECKSUM_SHA256,reinterpret_cast<const guchar*>(data),size);g_free(data);
        std::ofstream manifest(std::string(argv[2])+".manifest.json");
        manifest<<"{\"schema\":1,\"gpu\":\""<<arch<<"\",\"offload_copy\":false,\"precision\":\"fp16\",\"sha256\":\""<<hash<<"\"}\n";g_free(hash);
        if(!manifest) throw std::runtime_error("Cannot write model manifest");
        program.print();
        return 0;
    } catch(const std::exception& e) {std::cerr<<"Model preparation failed: "<<e.what()<<"\n";return 2;}
}
