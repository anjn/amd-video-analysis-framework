#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/analytics/analytics.h>
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <algorithm>

namespace {
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
unsigned rss_kib() {
    std::ifstream status("/proc/self/status");std::string line;
    while(std::getline(status,line)) if(line.rfind("VmRSS:",0)==0) return std::stoul(line.substr(6));
    throw std::runtime_error("Missing RSS counter");
}
struct Audit {std::atomic<unsigned> frames{0},bad{0};};
GstPadProbeReturn audit(GstPad*,GstPadProbeInfo* info,gpointer user) {
    auto& a=*static_cast<Audit*>(user);auto* buffer=GST_PAD_PROBE_INFO_BUFFER(info);
    auto* meta=gst_buffer_get_analytics_relation_meta(buffer);unsigned count=0;
    if(meta) {gpointer cursor=nullptr;GstAnalyticsMtd mtd{};
        while(gst_analytics_relation_meta_iterate(meta,&cursor,gst_analytics_od_mtd_get_mtd_type(),&mtd)) ++count;
    }
    if(count!=6)++a.bad;++a.frames;return GST_PAD_PROBE_OK;
}
}
int main(int argc,char** argv) {
    gst_init(&argc,&argv);GstElement* pipeline=nullptr;GstElement* source=nullptr;GstMemory* memory=nullptr;
    try {
        require(argc==2,"Usage: amdva-dlstreamer-stress EVENTS.jsonl");
        gchar* escaped=g_strescape(argv[1],nullptr);
        const std::string description="appsrc name=input format=time block=true max-buffers=4 caps=\"video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1\" ! gvatrack tracking-type=zero-term-imageless ! gvametaconvert add-empty-results=true ! gvametapublish file-format=json-lines file-path=\""+std::string(escaped)+"\" ! fakesink name=output sync=false";
        g_free(escaped);GError* error=nullptr;pipeline=gst_parse_launch(description.c_str(),&error);
        if(error) {std::string message=error->message;g_clear_error(&error);throw std::runtime_error(message);}
        require(pipeline,"Pipeline construction failed");source=gst_bin_get_by_name(GST_BIN(pipeline),"input");
        auto* sink=gst_bin_get_by_name(GST_BIN(pipeline),"output");auto* pad=gst_element_get_static_pad(sink,"sink");
        Audit checked;gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_BUFFER,audit,&checked,nullptr);
        gst_object_unref(pad);gst_object_unref(sink);
        std::array<GQuark,80> labels{};std::array<float,80> scores{};
        for(unsigned i=0;i<80;i++) labels[i]=g_quark_from_string(("class"+std::to_string(i)).c_str());
        memory=gst_allocator_alloc(nullptr,1920*1080*3/2,nullptr);require(memory,"Fixture allocation failed");
        require(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"Pipeline start failed");
        unsigned baseline=0,peak=0;
        for(unsigned i=0;i<3000;i++) {
            auto* buffer=gst_buffer_new();gst_buffer_append_memory(buffer,gst_memory_ref(memory));
            GST_BUFFER_PTS(buffer)=i*(GST_SECOND/30);GST_BUFFER_DURATION(buffer)=GST_SECOND/30;
            auto* meta=gst_buffer_add_analytics_relation_meta(buffer);GstAnalyticsClsMtd cls{};
            require(gst_analytics_relation_meta_add_cls_mtd(meta,80,scores.data(),labels.data(),&cls),"Class descriptor failed");
            gst_analytics_mtd_set_semantic_tag(reinterpret_cast<GstAnalyticsMtd*>(&cls),"class_descriptor");
            for(unsigned j=0;j<6;j++) {
                GstAnalyticsODMtd od{};
                require(gst_analytics_relation_meta_add_od_mtd(meta,labels[j],100*j,200,50,60,.75,&od),"Detection metadata failed");
                require(gst_analytics_relation_meta_set_relation(meta,GST_ANALYTICS_REL_TYPE_RELATE_TO,od.id,cls.id),"Class relation failed");
            }
            require(gst_app_src_push_buffer(GST_APP_SRC(source),buffer)==GST_FLOW_OK,"Metadata push failed");
            if(i==499) baseline=peak=rss_kib();
            if(i>=500 && i%100==0) peak=std::max(peak,rss_kib());
        }
        gst_app_src_end_of_stream(GST_APP_SRC(source));auto* bus=gst_element_get_bus(pipeline);
        auto* message=gst_bus_timed_pop_filtered(bus,30*GST_SECOND,static_cast<GstMessageType>(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
        const bool eos=message && GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS;
        if(message)gst_message_unref(message);gst_object_unref(bus);peak=std::max(peak,rss_kib());
        require(eos && checked.frames==3000 && checked.bad==0,"Metadata stream did not drain correctly");
        const auto growth=peak-baseline;
        gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);pipeline=nullptr;
        gst_object_unref(source);source=nullptr;gst_memory_unref(memory);memory=nullptr;
        std::cout<<"{\"frames\":3000,\"detections_per_frame\":6,\"rss_growth_kib\":"<<growth
                 <<",\"limit_kib\":4096,\"passed\":"<<(growth<=4096?"true":"false")<<"}\n";
        return growth<=4096?0:2;
    } catch(const std::exception& error) {
        if(pipeline){gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);}
        if(source)gst_object_unref(source);if(memory)gst_memory_unref(memory);
        std::cerr<<"DL Streamer metadata stress failed: "<<error.what()<<"\n";return 2;
    }
}
