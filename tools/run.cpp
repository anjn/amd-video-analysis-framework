#include "amdva/runtime.hpp"
#include <gst/gst.h>
#include <gst/analytics/analytics.h>
#include <gst/video/video.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <dlfcn.h>
namespace {
volatile std::sig_atomic_t stopped=0;
void signal_handler(int) {stopped=1;}
struct Frame {GstMemory* memory;GstClockTime pts;std::chrono::steady_clock::time_point started;};
struct Audit {
    uint64_t input{},output{},detections{},violations{};
    std::unordered_map<GstClockTime,Frame> pending;
    std::vector<double> latency;
    void (*start_copy_audit)(){};
};
GstPadProbeReturn input_probe(GstPad*,GstPadProbeInfo* info,gpointer user) {
    auto& a=*static_cast<Audit*>(user);auto* b=GST_PAD_PROBE_INFO_BUFFER(info);if(!b)return GST_PAD_PROBE_OK;
    ++a.input;const auto pts=GST_BUFFER_PTS(b);
    if(a.input==1 && a.start_copy_audit) a.start_copy_audit();
    if(!GST_CLOCK_TIME_IS_VALID(pts) || !gst_buffer_n_memory(b) || a.pending.count(pts)) {++a.violations;return GST_PAD_PROBE_OK;}
    a.pending.emplace(pts,Frame{gst_buffer_peek_memory(b,0),pts,std::chrono::steady_clock::now()});
    return GST_PAD_PROBE_OK;
}
GstPadProbeReturn output_probe(GstPad*,GstPadProbeInfo* info,gpointer user) {
    auto& a=*static_cast<Audit*>(user);auto* b=GST_PAD_PROBE_INFO_BUFFER(info);if(!b)return GST_PAD_PROBE_OK;
    ++a.output;auto found=a.pending.find(GST_BUFFER_PTS(b));
    if(found==a.pending.end() || !gst_buffer_n_memory(b) || gst_buffer_peek_memory(b,0)!=found->second.memory) ++a.violations;
    else {a.latency.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-found->second.started).count());a.pending.erase(found);}
    auto* analytics=gst_buffer_get_analytics_relation_meta(b);
    if(analytics) {
        gpointer cursor=nullptr;GstAnalyticsMtd mtd;
        while(gst_analytics_relation_meta_iterate(analytics,&cursor,gst_analytics_od_mtd_get_mtd_type(),&mtd)) ++a.detections;
    }
    return GST_PAD_PROBE_OK;
}
std::string quoted(const char* s) {gchar* escaped=g_strescape(s,nullptr);std::string q="\""+std::string(escaped)+"\"";g_free(escaped);return q;}
double percentile(std::vector<double>& data,double p) {if(data.empty())return 0;std::sort(data.begin(),data.end());return data[std::min(data.size()-1,size_t(p*(data.size()-1)))];}
}
int main(int argc,char** argv) {
    gst_init(&argc,&argv);
    if(argc<4) {
        std::cerr<<"Usage: amdva-run INPUT.h264 MODEL.mxr MODEL-INFO.json [--amd-only] [--duration=SECONDS] [--events=PATH]\n";return 1;
    }
    bool dlstreamer=true;unsigned duration=0;std::string events="events.jsonl";
    for(int i=4;i<argc;i++) {
        const std::string option=argv[i];
        if(option=="--amd-only")dlstreamer=false;
        else if(option.rfind("--duration=",0)==0) {
            try {size_t used=0;const auto value=std::stoul(option.substr(11),&used);if(used!=option.size()-11 || value>86400 || !value)throw std::invalid_argument("range");duration=unsigned(value);}
            catch(...) {std::cerr<<"Invalid duration (1..86400 seconds)\n";return 1;}
        } else if(option.rfind("--events=",0)==0) events=option.substr(9);
        else {std::cerr<<"Unknown option: "<<option<<"\n";return 1;}
    }
    if(duration && std::string(argv[1]).find('%')!=std::string::npos) {std::cerr<<"Loop input must have a literal filename without %\n";return 1;}
    const std::string input=(duration?"multifilesrc loop=true stop-index=0 caps=\"video/x-h264,stream-format=byte-stream,framerate=30/1\" location=":"filesrc location=")+quoted(argv[1]);
    const std::string pipeline=input+" ! h264parse ! vah264dec ! "
        "video/x-raw(memory:VAMemory),format=NV12 ! amdpreproc name=preproc ! amdmigraphx name=infer model="+
        quoted(argv[2])+" model-info="+quoted(argv[3])+ (dlstreamer?
        " ! gvatrack tracking-type=zero-term-imageless ! gvametaconvert add-empty-results=true ! gvametapublish file-format=json-lines file-path="+quoted(events.c_str()):"")+
        " ! fakesink name=output sync="+(duration?"true":"false");
    GError* error=nullptr;auto* graph=gst_parse_launch(pipeline.c_str(),&error);
    if(error || !graph) {std::cerr<<(error?error->message:"Pipeline construction failed")<<"\n";g_clear_error(&error);if(graph)gst_object_unref(graph);return 2;}
    Audit audit;
    audit.start_copy_audit=reinterpret_cast<void(*)()>(dlsym(RTLD_DEFAULT,"amdva_audit_start"));
    auto* pre=gst_bin_get_by_name(GST_BIN(graph),"preproc");auto* output=gst_bin_get_by_name(GST_BIN(graph),"output");
    auto* inpad=gst_element_get_static_pad(pre,"sink");auto* outpad=gst_element_get_static_pad(output,"sink");
    gst_pad_add_probe(inpad,GST_PAD_PROBE_TYPE_BUFFER,input_probe,&audit,nullptr);
    gst_pad_add_probe(outpad,GST_PAD_PROBE_TYPE_BUFFER,output_probe,&audit,nullptr);
    gst_object_unref(inpad);gst_object_unref(outpad);gst_object_unref(pre);gst_object_unref(output);
    auto* bus=gst_element_get_bus(graph);bool failed=false,eos=false,eos_sent=false;
    const auto started=std::chrono::steady_clock::now();
    std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
    if(gst_element_set_state(graph,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE)failed=true;
    while(!failed && !eos) {
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        if(!eos_sent && (stopped || (duration && elapsed>=duration))) {
            gst_element_send_event(graph,gst_event_new_eos());stopped=0;eos_sent=true;
        }
        if(eos_sent && duration && elapsed>duration+10) {std::cerr<<"EOS drain timed out\n";failed=true;break;}
        auto* message=gst_bus_timed_pop_filtered(bus,100*GST_MSECOND,static_cast<GstMessageType>(GST_MESSAGE_ERROR|GST_MESSAGE_EOS));
        if(!message)continue;
        if(GST_MESSAGE_TYPE(message)==GST_MESSAGE_ERROR) {
            gchar* debug{};GError* e{};gst_message_parse_error(message,&e,&debug);
            std::cerr<<e->message<<"\n"<<(debug?debug:"")<<"\n";g_clear_error(&e);g_free(debug);failed=true;
        } else eos=true;
        gst_message_unref(message);
    }
    gst_element_set_state(graph,GST_STATE_NULL);gst_object_unref(bus);gst_object_unref(graph);
    auto errors=reinterpret_cast<unsigned long long(*)()>(dlsym(RTLD_DEFAULT,"amdva_audit_errors"));
    auto copies=reinterpret_cast<unsigned long long(*)()>(dlsym(RTLD_DEFAULT,"amdva_audit_copies"));
    auto bytes=reinterpret_cast<unsigned long long(*)()>(dlsym(RTLD_DEFAULT,"amdva_audit_bytes"));
    auto stop_audit=reinterpret_cast<void(*)()>(dlsym(RTLD_DEFAULT,"amdva_audit_stop"));
    if(stop_audit)stop_audit();
    const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    const bool passed=!failed && eos && audit.input>0 && audit.input==audit.output && audit.violations==0 && audit.pending.empty();
    const bool copy_passed=errors && copies && bytes && errors()==0 && copies()==audit.output && bytes()==audit.output*7204;
    std::cout<<"{\"schema\":1,\"dlstreamer\":"<<(dlstreamer?"true":"false")<<",\"frames_in\":"<<audit.input
        <<",\"frames_out\":"<<audit.output<<",\"detections\":"<<audit.detections<<",\"identity_violations\":"<<audit.violations
        <<",\"fps\":"<<(seconds?audit.output/seconds:0)<<",\"p95_ms\":"<<percentile(audit.latency,0.95)
        <<",\"p99_ms\":"<<percentile(audit.latency,0.99)<<",\"buffer_identity_passed\":"<<(passed?"true":"false")
        <<",\"copy_audit_present\":"<<(errors?"true":"false")<<",\"copy_audit_passed\":"<<(copy_passed?"true":"false")
        <<",\"copy_violations\":"<<(errors?errors():0)<<",\"readback_bytes\":"<<(bytes?bytes():0)
        <<",\"pipeline_zero_copy_verified\":"<<(passed && copy_passed?"true":"false")<<"}\n";
    // Identity is only one piece of evidence; copy tracing remains required.
    return passed && (!errors || copy_passed)?0:2;
}
