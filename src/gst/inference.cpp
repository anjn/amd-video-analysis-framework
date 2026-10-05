#include "amdva/tensor_meta.hpp"
#include <gst/base/gstbasetransform.h>
#include <gst/analytics/analytics.h>
#include <gst/video/video.h>
#include <json-glib/json-glib.h>
#include <migraphx/migraphx.hpp>
#include <fstream>
#include <array>
#include <cmath>
#include <algorithm>

GST_DEBUG_CATEGORY_STATIC(infer_debug);
#define GST_CAT_DEFAULT infer_debug
namespace {
constexpr unsigned max_detections=300;
struct Detection { float x,y,w,h,score; int class_id; };
struct Results { unsigned count; Detection boxes[max_detections]; };
static_assert(sizeof(Detection)==24);
static const char* post_source=R"(
struct Box {float x,y,w,h,score; int cls;};
struct Result {unsigned count; Box boxes[300];};
extern "C" __global__ void decode(const float* raw, unsigned n, float threshold, Box* boxes) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
 if(i>=n) return;
 Box b; b.x=raw[i];b.y=raw[n+i];b.w=raw[2*n+i];b.h=raw[3*n+i];
 b.score=0;b.cls=0;
 for(unsigned c=0;c<80;c++) {float p=raw[(4+c)*n+i];if(p>b.score){b.score=p;b.cls=c;}}
 b.x-=b.w/2;b.y-=b.h/2;
 if(!isfinite(b.x)||!isfinite(b.y)||!isfinite(b.w)||!isfinite(b.h)||!isfinite(b.score)||b.w<=0||b.h<=0||b.score<threshold) b.score=0;
 boxes[i]=b;
}
extern "C" __global__ void nms(Box* boxes,unsigned n,float threshold,Result* result) {
 if(blockIdx.x || threadIdx.x) return;
 result->count=0;
 for(unsigned k=0;k<300;k++) {
  int best=-1; float score=0;
  for(unsigned i=0;i<n;i++) if(boxes[i].score>score){best=i;score=boxes[i].score;}
  if(best<0) break;
  Box b=boxes[best]; result->boxes[result->count++]=b;boxes[best].score=0;
  for(unsigned i=0;i<n;i++) {
   Box c=boxes[i]; if(c.score<=0||c.cls!=b.cls) continue;
   float w=fmaxf(0,fminf(b.x+b.w,c.x+c.w)-fmaxf(b.x,c.x));
   float h=fmaxf(0,fminf(b.y+b.h,c.y+c.h)-fmaxf(b.y,c.y));
   float intersection=w*h,area=b.w*b.h+c.w*c.h-intersection;
   if(area>0 && intersection/area>threshold) boxes[i].score=0;
  }
 }
})";
struct Parser {
    JsonParser* parser{json_parser_new()};
    explicit Parser(const std::string& path) {
        GError* e=nullptr;
        if(!json_parser_load_from_file(parser,path.c_str(),&e)) {
            const std::string message=e?e->message:"JSON load failed";
            g_clear_error(&e); g_object_unref(parser);parser=nullptr;throw std::runtime_error(message);
        }
        if(!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {g_object_unref(parser);parser=nullptr;throw std::runtime_error("JSON root must be an object");}
    }
    ~Parser(){if(parser)g_object_unref(parser);}
    JsonObject* object() const {return json_node_get_object(json_parser_get_root(parser));}
};
static const char* required_string(JsonObject* o,const char* key) {
    if(!json_object_has_member(o,key) || json_node_get_value_type(json_object_get_member(o,key))!=G_TYPE_STRING)
        throw std::runtime_error(std::string("Missing string: ")+key);
    return json_object_get_string_member(o,key);
}
static JsonObject* required_object(JsonObject* o,const char* key) {
    if(!json_object_has_member(o,key) || !JSON_NODE_HOLDS_OBJECT(json_object_get_member(o,key)))
        throw std::runtime_error(std::string("Missing object: ")+key);
    return json_object_get_object_member(o,key);
}
static JsonArray* required_array(JsonObject* o,const char* key) {
    if(!json_object_has_member(o,key) || !JSON_NODE_HOLDS_ARRAY(json_object_get_member(o,key)))
        throw std::runtime_error(std::string("Missing array: ")+key);
    return json_object_get_array_member(o,key);
}
static std::string sha256_file(const char* path) {
    gchar* data{}; gsize size{}; GError* e=nullptr;
    if(!g_file_get_contents(path,&data,&size,&e)) {
        std::string error=e?e->message:"Read failed";g_clear_error(&e);throw std::runtime_error(error);
    }
    gchar* hash=g_compute_checksum_for_data(G_CHECKSUM_SHA256,reinterpret_cast<const guchar*>(data),size);
    std::string result=hash;g_free(hash);g_free(data);return result;
}
struct InferState {
    migraphx::program program;
    std::string input_name;
    migraphx::shape input_shape;
    std::vector<std::string> labels;
    std::vector<std::pair<std::string,std::unique_ptr<amdva::DeviceBuffer>>> bindings;
    std::unique_ptr<amdva::Kernel> decode,nms;
    std::unique_ptr<amdva::DeviceBuffer> boxes,result;
    unsigned candidates{};
    unsigned width{},height{};
};
}
typedef struct {GstBaseTransform parent;gchar* model;gchar* model_info;gint device;gfloat confidence,nms;gboolean strict;InferState* state;} AmdMigraphx;
typedef struct {GstBaseTransformClass parent;} AmdMigraphxClass;
G_DEFINE_TYPE(AmdMigraphx,amd_migraphx,GST_TYPE_BASE_TRANSFORM)
enum {P_NONE,P_MODEL,P_INFO,P_DEVICE,P_CONF,P_NMS,P_STRICT};
static GstStaticPadTemplate sink=GST_STATIC_PAD_TEMPLATE("sink",GST_PAD_SINK,GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]"));
static GstStaticPadTemplate src=GST_STATIC_PAD_TEMPLATE("src",GST_PAD_SRC,GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]"));
static void set_property(GObject* o,guint p,const GValue* v,GParamSpec* spec) {
    auto* s=reinterpret_cast<AmdMigraphx*>(o);
    if(GST_STATE(s)>GST_STATE_READY) {GST_WARNING_OBJECT(s,"Properties can only change in NULL or READY");return;}
    switch(p) {
    case P_MODEL:g_free(s->model);s->model=g_value_dup_string(v);break;
    case P_INFO:g_free(s->model_info);s->model_info=g_value_dup_string(v);break;
    case P_DEVICE:s->device=g_value_get_int(v);break;
    case P_CONF:s->confidence=g_value_get_float(v);break;
    case P_NMS:s->nms=g_value_get_float(v);break;
    case P_STRICT:s->strict=g_value_get_boolean(v);break;
    default:G_OBJECT_WARN_INVALID_PROPERTY_ID(o,p,spec);
    }
}
static void get_property(GObject* o,guint p,GValue* v,GParamSpec* spec) {
    auto* s=reinterpret_cast<AmdMigraphx*>(o);
    switch(p) {
    case P_MODEL:g_value_set_string(v,s->model);break;
    case P_INFO:g_value_set_string(v,s->model_info);break;
    case P_DEVICE:g_value_set_int(v,s->device);break;
    case P_CONF:g_value_set_float(v,s->confidence);break;
    case P_NMS:g_value_set_float(v,s->nms);break;
    case P_STRICT:g_value_set_boolean(v,s->strict);break;
    default:G_OBJECT_WARN_INVALID_PROPERTY_ID(o,p,spec);
    }
}
static gboolean start(GstBaseTransform* t) {
    auto* s=reinterpret_cast<AmdMigraphx*>(t);
    try {
        if(!s->strict || !s->model || !s->model_info) throw std::runtime_error("Strict mode, model and model-info are required");
        const auto arch=amdva::gpu_arch(s->device);
        Parser manifest(std::string(s->model)+".manifest.json");auto* m=manifest.object();
        if(!json_object_has_member(m,"schema") || json_node_get_value_type(json_object_get_member(m,"schema"))!=G_TYPE_INT64 || json_object_get_int_member(m,"schema")!=1 ||
           !json_object_has_member(m,"offload_copy") || json_node_get_value_type(json_object_get_member(m,"offload_copy"))!=G_TYPE_BOOLEAN || json_object_get_boolean_member(m,"offload_copy") ||
           arch!=required_string(m,"gpu") || sha256_file(s->model)!=required_string(m,"sha256"))
            throw std::runtime_error("Model manifest is incompatible: GPU, hash or device I/O contract");
        auto state=std::make_unique<InferState>();
        Parser info(s->model_info);auto* config=info.object();
        if(std::string(required_string(config,"family"))!="yolov8") throw std::runtime_error("PoC supports YOLOv8 COCO detection only");
        auto* input=required_object(config,"input");
        auto* output=required_object(config,"output");
        if(!input || !output || std::string(required_string(input,"format"))!="NCHW" ||
           std::string(required_string(input,"channel_order"))!="RGB" ||
           std::string(required_string(output,"format"))!="cxcywh" || json_object_get_int_member(output,"num_classes")!=80)
            throw std::runtime_error("Unsupported YOLO input/output convention");
        auto* normalize=required_array(input,"normalize");
        if(json_array_get_length(normalize)!=2 || json_array_get_double_element(normalize,0)!=0 || json_array_get_double_element(normalize,1)!=1)
            throw std::runtime_error("Expected RGB normalization to [0,1]");
        auto* shape=required_array(input,"shape");
        if(!shape || json_array_get_length(shape)!=4 || json_array_get_int_element(shape,0)!=1 || json_array_get_int_element(shape,1)!=3)
            throw std::runtime_error("Expected static [1,3,H,W] input");
        state->height=json_array_get_int_element(shape,2);state->width=json_array_get_int_element(shape,3);
        if(state->height!=640 || state->width!=640) throw std::runtime_error("Initial model shape is fixed to 640x640");
        gchar* dir=g_path_get_dirname(s->model_info);const auto labels_path=std::string(dir)+"/"+required_string(config,"labels");g_free(dir);
        std::ifstream labels(labels_path);std::string line;
        while(std::getline(labels,line)) {if(!line.empty() && line.back()=='\r')line.pop_back();state->labels.push_back(line);}
        if(state->labels.size()!=80) throw std::runtime_error("Expected 80 COCO labels relative to model-info");
        state->program=migraphx::load(s->model);
        auto params=state->program.get_parameter_shapes();
        for(const char* name:params.names()) {
            auto sh=params[name];
            if(sh.type()==migraphx_shape_float_type && sh.lengths()==std::vector<size_t>{1,3,640,640}) {
                if(!state->input_name.empty()) throw std::runtime_error("Ambiguous model image input");
                state->input_name=name;state->input_shape=sh;
            } else if(std::string(name).find("#output_")!=std::string::npos && sh.type()==migraphx_shape_float_type && sh.lengths()==std::vector<size_t>{1,84,8400}) {
                state->bindings.emplace_back(name,std::make_unique<amdva::DeviceBuffer>(sh.bytes()));
            } else throw std::runtime_error(std::string("Unsupported additional model input: ")+name);
        }
        if(state->input_name.empty()) throw std::runtime_error("Missing float32 NCHW input");
        auto outputs=state->program.get_output_shapes();
        if(outputs.size()!=1 || outputs[0].type()!=migraphx_shape_float_type ||
           outputs[0].lengths()!=std::vector<size_t>{1,84,8400} ||
           outputs[0].strides()!=std::vector<size_t>{705600,8400,1}) throw std::runtime_error("Expected contiguous YOLOv8 [1,84,8400] float output");
        state->candidates=8400;
        state->decode=std::make_unique<amdva::Kernel>(post_source,"decode",arch);
        state->nms=std::make_unique<amdva::Kernel>(post_source,"nms",arch);
        state->boxes=std::make_unique<amdva::DeviceBuffer>(8400*sizeof(Detection));
        state->result=std::make_unique<amdva::DeviceBuffer>(sizeof(Results));
        delete s->state;s->state=state.release();return TRUE;
    } catch(const std::exception& e) {
        GST_ELEMENT_ERROR(s,LIBRARY,INIT,("AMD inference initialization failed"),("%s",e.what()));return FALSE;
    }
}
static gboolean stop(GstBaseTransform* t) {
    auto* s=reinterpret_cast<AmdMigraphx*>(t);(void)hipSetDevice(s->device);if(s->state)(void)hipDeviceSynchronize();delete s->state;s->state=nullptr;return TRUE;
}
static GstFlowReturn transform(GstBaseTransform* t,GstBuffer* buffer) {
    auto* s=reinterpret_cast<AmdMigraphx*>(t);
    try {
        auto* meta=amdva::get_tensor_meta(buffer);
        if(!meta || !meta->tensor || !*meta->tensor) throw std::runtime_error("Missing GPU tensor; connect amdpreproc upstream");
        const auto tensor=*meta->tensor;auto& state=*s->state;
        if(tensor->device!=s->device || tensor->width!=state.width || tensor->height!=state.height)
            throw std::runtime_error("GPU tensor device/shape mismatch");
        amdva::hip_check(hipSetDevice(s->device),"Inference device selection");
        amdva::hip_check(hipEventSynchronize(tensor->ready),"Input tensor readiness");
        migraphx::program_parameters params;
        params.add(state.input_name.c_str(),migraphx::argument(state.input_shape,tensor->buffer->data()));
        auto shapes=state.program.get_parameter_shapes();
        for(auto& binding:state.bindings) params.add(binding.first.c_str(),migraphx::argument(shapes[binding.first.c_str()],binding.second->data()));
        auto outputs=state.program.eval(params);
        // Device-I/O evaluation may enqueue work on MIGraphX's non-blocking
        // stream. Default-stream postprocessing does not implicitly wait for it.
        // Complete inference before reading outputs or releasing the input owner.
        amdva::hip_check(hipDeviceSynchronize(),"MIGraphX device-I/O completion");
        auto* raw=reinterpret_cast<float*>(outputs[0].data());
        hipPointerAttribute_t attrs{};amdva::hip_check(hipPointerGetAttributes(&attrs,raw),"Model output pointer check");
        if(attrs.type!=hipMemoryTypeDevice || attrs.device!=s->device) throw std::runtime_error("MIGraphX output is not on the selected GPU");
        unsigned n=state.candidates;void* boxes=state.boxes->data();void* result=state.result->data();
        void* decode_args[]={&raw,&n,&s->confidence,&boxes};state.decode->launch(n,decode_args);
        void* nms_args[]={&boxes,&n,&s->nms,&result};state.nms->launch(1,nms_args);
        Results detected{};amdva::hip_check(hipMemcpy(&detected,result,sizeof(detected),hipMemcpyDeviceToHost),"Compact detection readback");
        if(detected.count>max_detections) throw std::runtime_error("Invalid GPU detection count");
        auto* analytics=gst_buffer_add_analytics_relation_meta(buffer);
        if(!analytics) throw std::runtime_error("Cannot allocate GstAnalytics metadata");
        std::array<GQuark,80> labels{};std::array<float,80> scores{};
        for(unsigned i=0;i<80;i++) labels[i]=g_quark_from_string(state.labels[i].c_str());
        GstAnalyticsClsMtd descriptor{};
        if(!gst_analytics_relation_meta_add_cls_mtd(analytics,80,scores.data(),labels.data(),&descriptor))
            throw std::runtime_error("Cannot attach class descriptor");
        gst_analytics_mtd_set_semantic_tag(reinterpret_cast<GstAnalyticsMtd*>(&descriptor),"class_descriptor");
        const float sx=float(tensor->original_width)/tensor->scaled_width;
        const float sy=float(tensor->original_height)/tensor->scaled_height;
        for(unsigned i=0;i<detected.count;i++) {
            const auto& b=detected.boxes[i];
            if(b.class_id<0 || b.class_id>=80) throw std::runtime_error("Invalid GPU class ID");
            // Clip in double precision before integer conversion, including
            // finite but out-of-range model coordinates.
            const int x=int(std::clamp(std::floor((double(b.x)-tensor->pad_x)*sx),0.0,double(tensor->original_width)));
            const int y=int(std::clamp(std::floor((double(b.y)-tensor->pad_y)*sy),0.0,double(tensor->original_height)));
            const int right=int(std::clamp(std::ceil((double(b.x)+b.w-tensor->pad_x)*sx),0.0,double(tensor->original_width)));
            const int bottom=int(std::clamp(std::ceil((double(b.y)+b.h-tensor->pad_y)*sy),0.0,double(tensor->original_height)));
            if(right<=x || bottom<=y) continue;
            GstAnalyticsODMtd od{};
            if(!gst_analytics_relation_meta_add_od_mtd(analytics,g_quark_from_string(state.labels[b.class_id].c_str()),x,y,right-x,bottom-y,b.score,&od))
                throw std::runtime_error("Cannot attach detection");
            if(!gst_analytics_relation_meta_set_relation(analytics,GST_ANALYTICS_REL_TYPE_RELATE_TO,od.id,descriptor.id))
                throw std::runtime_error("Cannot relate detection to class descriptor");
        }
        GST_LOG_OBJECT(s,"detections=%u, compact readback=%zu bytes",detected.count,sizeof(detected));
        gst_buffer_remove_meta(buffer,&meta->meta);return GST_FLOW_OK;
    } catch(const std::exception& e) {
        (void)hipDeviceSynchronize();
        GST_ELEMENT_ERROR(s,STREAM,FAILED,("AMD inference failed"),("%s",e.what()));return GST_FLOW_ERROR;
    }
}
static void finalize(GObject* o) {
    auto* s=reinterpret_cast<AmdMigraphx*>(o);if(s->state)(void)hipSetDevice(s->device);delete s->state;g_free(s->model);g_free(s->model_info);
    G_OBJECT_CLASS(amd_migraphx_parent_class)->finalize(o);
}
static void amd_migraphx_class_init(AmdMigraphxClass* klass) {
    auto* object=G_OBJECT_CLASS(klass);object->set_property=set_property;object->get_property=get_property;object->finalize=finalize;
    const auto f=static_cast<GParamFlags>(G_PARAM_READWRITE|G_PARAM_STATIC_STRINGS|GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(object,P_MODEL,g_param_spec_string("model","Model","Prepared device-I/O MXR with manifest",nullptr,f));
    g_object_class_install_property(object,P_INFO,g_param_spec_string("model-info","Model info","YOLOv8 metadata JSON",nullptr,f));
    g_object_class_install_property(object,P_DEVICE,g_param_spec_int("device","GPU device","HIP device index",0,16,0,f));
    g_object_class_install_property(object,P_CONF,g_param_spec_float("confidence-threshold","Confidence threshold","Minimum detection score",0,1,0.25f,f));
    g_object_class_install_property(object,P_NMS,g_param_spec_float("nms-threshold","NMS threshold","Class-aware overlap threshold",0,1,0.45f,f));
    g_object_class_install_property(object,P_STRICT,g_param_spec_boolean("require-zero-copy","Strict mode","Require device input and output",TRUE,f));
    auto* element=GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element,"AMD MIGraphX inference","Filter/Video","GPU inference and NMS with GstAnalytics output","AMD");
    gst_element_class_add_static_pad_template(element,&sink);gst_element_class_add_static_pad_template(element,&src);
    auto* base=GST_BASE_TRANSFORM_CLASS(klass);base->start=start;base->stop=stop;base->transform_ip=transform;
    GST_DEBUG_CATEGORY_INIT(infer_debug,"amdmigraphx",0,"AMD GPU inference");
}
static void amd_migraphx_init(AmdMigraphx* s) {
    s->confidence=0.25f;s->nms=0.45f;s->strict=TRUE;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(s),TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(s),FALSE);
}
