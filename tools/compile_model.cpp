#include "amdva/runtime.hpp"
#include <migraphx/migraphx.hpp>
#include <glib.h>
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    if(argc!=3 && argc!=4) {std::cerr<<"Usage: amdva-compile-model MODEL.onnx OUTPUT.mxr [--batch=N]\n";return 1;}
    try {
        unsigned batch=1;
        if(argc==4) {const std::string arg=argv[3];if(arg.rfind("--batch=",0)!=0) throw std::runtime_error("Expected --batch=N");size_t used=0;batch=std::stoul(arg.substr(8),&used);if(used!=arg.size()-8)throw std::runtime_error("Invalid batch argument");}
        if(batch!=1 && batch!=2 && batch!=4 && batch!=8 && batch!=16) throw std::runtime_error("Invalid batch size");
        const auto arch=amdva::gpu_arch(0);
        migraphx::onnx_options options;options.set_default_dim_value(1);options.set_input_parameter_shape("images",{batch,3,640,640});
        auto program=migraphx::parse_onnx(argv[1],options);
        auto shapes=program.get_parameter_shapes();bool image=false;
        for(const char* name:shapes.names()) if(shapes[name].lengths()==std::vector<size_t>{batch,3,640,640}) image=true;
        auto outputs=program.get_output_shapes();
        if(outputs.size()!=1 || outputs[0].lengths()!=std::vector<size_t>{batch,84,8400}) throw std::runtime_error("Expected YOLOv8 [B,84,8400] output");
        if(!image) throw std::runtime_error("ONNX must have [B,3,640,640] input; export dynamic-batch weights first");
        migraphx::quantize_fp16(program);
        migraphx::compile_options compile;compile.set_offload_copy(false);
        program.compile(migraphx::target("gpu"),compile);
        migraphx::save(program,argv[2]);
        gchar* data{};gsize size{};
        if(!g_file_get_contents(argv[2],&data,&size,nullptr)) throw std::runtime_error("Cannot hash compiled model");
        gchar* hash=g_compute_checksum_for_data(G_CHECKSUM_SHA256,reinterpret_cast<const guchar*>(data),size);g_free(data);
        std::ofstream manifest(std::string(argv[2])+".manifest.json");
        manifest<<"{\"schema\":1,\"gpu\":\""<<arch<<"\",\"offload_copy\":false,\"precision\":\"fp16\",\"batch\":"<<batch<<",\"sha256\":\""<<hash<<"\"}\n";g_free(hash);
        if(!manifest) throw std::runtime_error("Cannot write model manifest");
        program.print();
        return 0;
    } catch(const std::exception& e) {std::cerr<<"Model preparation failed: "<<e.what()<<"\n";return 2;}
}
