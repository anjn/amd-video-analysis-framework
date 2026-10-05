#include "amdva/tensor_meta.hpp"
#include "amdva/inference.hpp"
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
typedef struct {GstBaseTransform parent;gchar* model;gchar* model_info;gint device;gfloat confidence,nms;gboolean strict;amdva::BatchEngine* state;} AmdMigraphx;
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
        auto state=std::make_unique<amdva::BatchEngine>(s->model,s->model_info,1,s->device);
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
        if(tensor->device!=s->device || tensor->width!=640 || tensor->height!=640)
            throw std::runtime_error("GPU tensor device/shape mismatch");
        amdva::hip_check(hipSetDevice(s->device),"Inference device selection");
        amdva::hip_check(hipEventSynchronize(tensor->ready),"Input tensor readiness");
        const auto detected=state.infer(tensor->buffer->data(),1,s->confidence,s->nms)[0];
        auto* analytics=gst_buffer_add_analytics_relation_meta(buffer);
        if(!analytics) throw std::runtime_error("Cannot allocate GstAnalytics metadata");
        std::array<GQuark,80> labels{};std::array<float,80> scores{};
        for(unsigned i=0;i<80;i++) labels[i]=g_quark_from_string(state.labels()[i].c_str());
        GstAnalyticsClsMtd descriptor{};
        if(!gst_analytics_relation_meta_add_cls_mtd(analytics,80,scores.data(),labels.data(),&descriptor))
            throw std::runtime_error("Cannot attach class descriptor");
        gst_analytics_mtd_set_semantic_tag(reinterpret_cast<GstAnalyticsMtd*>(&descriptor),"class_descriptor");
        const float sx=float(tensor->original_width)/tensor->scaled_width;
        const float sy=float(tensor->original_height)/tensor->scaled_height;
        for(unsigned i=0;i<detected.size();i++) {
            const auto& b=detected[i];
            if(b.class_id<0 || b.class_id>=80) throw std::runtime_error("Invalid GPU class ID");
            // Clip in double precision before integer conversion, including
            // finite but out-of-range model coordinates.
            const int x=int(std::clamp(std::floor((double(b.x)-tensor->pad_x)*sx),0.0,double(tensor->original_width)));
            const int y=int(std::clamp(std::floor((double(b.y)-tensor->pad_y)*sy),0.0,double(tensor->original_height)));
            const int right=int(std::clamp(std::ceil((double(b.x)+b.w-tensor->pad_x)*sx),0.0,double(tensor->original_width)));
            const int bottom=int(std::clamp(std::ceil((double(b.y)+b.h-tensor->pad_y)*sy),0.0,double(tensor->original_height)));
            if(right<=x || bottom<=y) continue;
            GstAnalyticsODMtd od{};
            if(!gst_analytics_relation_meta_add_od_mtd(analytics,g_quark_from_string(state.labels()[b.class_id].c_str()),x,y,right-x,bottom-y,b.score,&od))
                throw std::runtime_error("Cannot attach detection");
            if(!gst_analytics_relation_meta_set_relation(analytics,GST_ANALYTICS_REL_TYPE_RELATE_TO,od.id,descriptor.id))
                throw std::runtime_error("Cannot relate detection to class descriptor");
        }
        GST_LOG_OBJECT(s,"detections=%zu, compact readback=%zu bytes",detected.size(),size_t(7204));
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
