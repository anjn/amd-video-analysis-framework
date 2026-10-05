#include "amdva/tensor_meta.hpp"
#include <gst/app/gstappsink.h>
#include <fstream>
#include <iostream>
#include <vector>
// Deliberate full-tensor download for an offline CPU correctness reference.
// This tool is not part of the zero-copy production/measurement path.
int main(int argc,char** argv) {
    gst_init(&argc,&argv);
    if(argc!=3) {std::cerr<<"Usage: amdva-dump-tensor INPUT.h264 OUTPUT.f32\n";return 1;}
    gchar* escaped=g_strescape(argv[1],nullptr);
    const auto launch="filesrc location=\""+std::string(escaped)+"\" ! h264parse ! vah264dec ! video/x-raw(memory:VAMemory),format=NV12 ! amdpreproc ! appsink name=output sync=false max-buffers=1 drop=true";
    g_free(escaped);GError* error=nullptr;auto* graph=gst_parse_launch(launch.c_str(),&error);
    if(error || !graph) {std::cerr<<(error?error->message:"Cannot build dump pipeline")<<"\n";g_clear_error(&error);if(graph)gst_object_unref(graph);return 2;}
    auto* sink=gst_bin_get_by_name(GST_BIN(graph),"output");GstSample* sample=nullptr;int status=0;
    try {
        if(gst_element_set_state(graph,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE)throw std::runtime_error("Dump pipeline failed to start");
        sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),10*GST_SECOND);
        if(!sample)throw std::runtime_error("No tensor from input");
        auto* m=amdva::get_tensor_meta(gst_sample_get_buffer(sample));
        if(!m || !m->tensor)throw std::runtime_error("Tensor metadata missing");
        const auto t=*m->tensor;
        std::vector<float> host(t->width*t->height*3);
        amdva::hip_check(hipSetDevice(t->device),"Dump device");
        amdva::hip_check(hipMemcpy(host.data(),t->buffer->data(),host.size()*sizeof(float),hipMemcpyDeviceToHost),"Offline reference readback");
        std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<char*>(host.data()),host.size()*sizeof(float));
        if(!out)throw std::runtime_error("Tensor file write failed");
        std::cout<<"{\"width\":"<<t->original_width<<",\"height\":"<<t->original_height
            <<",\"scaled_width\":"<<t->scaled_width<<",\"scaled_height\":"<<t->scaled_height
            <<",\"pad_x\":"<<t->pad_x<<",\"pad_y\":"<<t->pad_y<<",\"pts\":"<<GST_BUFFER_PTS(gst_sample_get_buffer(sample))<<"}\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";status=2;}
    if(sample)gst_sample_unref(sample);gst_element_set_state(graph,GST_STATE_NULL);gst_object_unref(sink);gst_object_unref(graph);return status;
}
