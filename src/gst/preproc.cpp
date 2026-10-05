#include "amdva/tensor_meta.hpp"
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <algorithm>
#include <cmath>

GST_DEBUG_CATEGORY_STATIC(preproc_debug);
#define GST_CAT_DEFAULT preproc_debug
static const char* kernel_source=R"(
extern "C" __global__ void pack(const unsigned char* bgra, unsigned offset, unsigned pitch,
 unsigned sw, unsigned sh, unsigned tw, unsigned th, unsigned px, unsigned py, float* tensor) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
 if(i>=tw*th) return;
 unsigned x=i%tw,y=i/tw;
 float r=114.0f/255.0f,g=r,b=r;
 if(x>=px && x<px+sw && y>=py && y<py+sh) {
   const unsigned char* pixel=bgra+offset+(y-py)*pitch+(x-px)*4;
   b=pixel[0]/255.0f; g=pixel[1]/255.0f; r=pixel[2]/255.0f;
 }
 tensor[i]=r; tensor[tw*th+i]=g; tensor[2*tw*th+i]=b;
})";
struct PreprocState {
    GstVideoInfo video{};
    GstVaDisplay* display{};
    std::shared_ptr<amdva::VaSession> va;
    std::unique_ptr<amdva::Kernel> pack;
    VASurfaceID scaled{VA_INVALID_SURFACE};
    unsigned scaled_width{},scaled_height{};
    std::unique_ptr<amdva::ImportedSurface> image;
    ~PreprocState() {
        image.reset();
        if(va) va->destroy_surface(scaled);
        pack.reset(); va.reset(); if(display) gst_object_unref(display);
    }
};
typedef struct { GstBaseTransform parent; gint device; guint width,height; gboolean strict; PreprocState* state; } AmdPreproc;
typedef struct { GstBaseTransformClass parent; } AmdPreprocClass;
G_DEFINE_TYPE(AmdPreproc,amd_preproc,GST_TYPE_BASE_TRANSFORM)
enum {P_NONE,P_DEVICE,P_WIDTH,P_HEIGHT,P_STRICT};
static GstStaticPadTemplate sink=GST_STATIC_PAD_TEMPLATE("sink",GST_PAD_SINK,GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]"));
static GstStaticPadTemplate src=GST_STATIC_PAD_TEMPLATE("src",GST_PAD_SRC,GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]"));
static void set_property(GObject* o,guint p,const GValue* v,GParamSpec* spec) {
    auto* self=reinterpret_cast<AmdPreproc*>(o);
    if(GST_STATE(self)>GST_STATE_READY) {GST_WARNING_OBJECT(self,"Properties can only change in NULL or READY");return;}
    switch(p) {
        case P_DEVICE:self->device=g_value_get_int(v);break;
        case P_WIDTH:self->width=g_value_get_uint(v);break;
        case P_HEIGHT:self->height=g_value_get_uint(v);break;
        case P_STRICT:self->strict=g_value_get_boolean(v);break;
        default:G_OBJECT_WARN_INVALID_PROPERTY_ID(o,p,spec);
    }
}
static void get_property(GObject* o,guint p,GValue* v,GParamSpec* spec) {
    auto* self=reinterpret_cast<AmdPreproc*>(o);
    switch(p) {
        case P_DEVICE:g_value_set_int(v,self->device);break;
        case P_WIDTH:g_value_set_uint(v,self->width);break;
        case P_HEIGHT:g_value_set_uint(v,self->height);break;
        case P_STRICT:g_value_set_boolean(v,self->strict);break;
        default:G_OBJECT_WARN_INVALID_PROPERTY_ID(o,p,spec);
    }
}
static gboolean start(GstBaseTransform* t) {
    auto* self=reinterpret_cast<AmdPreproc*>(t);
    if(!self->strict) {
        GST_ELEMENT_ERROR(self,LIBRARY,SETTINGS,("Only strict zero-copy mode is implemented"),(nullptr)); return FALSE;
    }
    delete self->state; self->state=new PreprocState;
    return TRUE;
}
static gboolean stop(GstBaseTransform* t) {
    auto* self=reinterpret_cast<AmdPreproc*>(t); delete self->state; self->state=nullptr; return TRUE;
}
static gboolean set_caps(GstBaseTransform* t,GstCaps* in,GstCaps*) {
    auto* self=reinterpret_cast<AmdPreproc*>(t);
    if(!self->state) return FALSE;
    auto& video=self->state->video;
    if(!gst_video_info_from_caps(&video,in) || GST_VIDEO_INFO_FORMAT(&video)!=GST_VIDEO_FORMAT_NV12 ||
       video.width%2 || video.height%2 || video.interlace_mode!=GST_VIDEO_INTERLACE_MODE_PROGRESSIVE) return FALSE;
    // VPE CSC is explicitly BT.709 limited-range. Reject other declared matrices/ranges.
    if((video.colorimetry.matrix!=GST_VIDEO_COLOR_MATRIX_UNKNOWN && video.colorimetry.matrix!=GST_VIDEO_COLOR_MATRIX_BT709) ||
       (video.colorimetry.range!=GST_VIDEO_COLOR_RANGE_UNKNOWN && video.colorimetry.range!=GST_VIDEO_COLOR_RANGE_16_235)) return FALSE;
    return TRUE;
}
static GstFlowReturn transform(GstBaseTransform* t,GstBuffer* buffer) {
    auto* self=reinterpret_cast<AmdPreproc*>(t);
    try {
        if(amdva::get_tensor_meta(buffer)) throw std::runtime_error("Input already carries an AMD tensor");
        auto* display=gst_va_buffer_peek_display(buffer);
        auto surface=gst_va_buffer_get_surface(buffer);
        if(!display || surface==VA_INVALID_SURFACE) throw std::runtime_error("Input has no VA surface");
        auto& state=*self->state;
        const auto arch=amdva::gpu_arch(self->device);
        if(!state.display) {
            state.display=GST_VA_DISPLAY(gst_object_ref(display));
            state.va=std::make_shared<amdva::VaSession>(static_cast<VADisplay>(gst_va_display_get_va_dpy(display)));
            state.pack=std::make_unique<amdva::Kernel>(kernel_source,"pack",arch);
        }
        if(display!=state.display) throw std::runtime_error("VA display changed; restart pipeline");
        auto tensor=std::make_shared<amdva::Tensor>();
        tensor->device=self->device; tensor->width=self->width; tensor->height=self->height;
        tensor->original_width=state.video.width; tensor->original_height=state.video.height;
        const double factor=std::min(double(self->width)/state.video.width,double(self->height)/state.video.height);
        tensor->scaled_width=std::max(1u,unsigned(std::lround(state.video.width*factor)));
        tensor->scaled_height=std::max(1u,unsigned(std::lround(state.video.height*factor)));
        tensor->pad_x=(self->width-tensor->scaled_width)/2; tensor->pad_y=(self->height-tensor->scaled_height)/2;
        // Retain one VA allocation and its HIP mapping for the pipeline lifetime.
        // Re-importing a newly allocated BO every frame causes retained shared
        // GPU allocations on this ROCm/Mesa combination, despite API teardown.
        if(state.scaled==VA_INVALID_SURFACE) {
            state.scaled=state.va->create_bgra(tensor->scaled_width,tensor->scaled_height);
            state.scaled_width=tensor->scaled_width;state.scaled_height=tensor->scaled_height;
        }
        if(state.scaled_width!=tensor->scaled_width || state.scaled_height!=tensor->scaled_height)
            throw std::runtime_error("Scaled geometry changed; restart pipeline");
        {
            const auto before=amdva::engine_time_ns("vpe");
            state.va->scale(surface,state.scaled,state.video.width,state.video.height,tensor->scaled_width,tensor->scaled_height);
            const auto after=amdva::engine_time_ns("vpe");
            if(after<=before) throw std::runtime_error("Dedicated VPE execution could not be observed");
            if(!state.image) state.image=std::make_unique<amdva::ImportedSurface>(state.va->export_surface(state.scaled));
            struct ReadGuard {
                bool pending{};
                ~ReadGuard(){if(pending)(void)hipDeviceSynchronize();}
            } read;
            const auto& layout=state.image->layout();
            tensor->buffer=std::make_shared<amdva::DeviceBuffer>(size_t(self->width)*self->height*3*sizeof(float));
            const uint8_t* pixels=state.image->data(); unsigned offset=layout.y_offset,pitch=layout.y_pitch;
            void* output=tensor->buffer->data();
            void* args[]={&pixels,&offset,&pitch,&tensor->scaled_width,&tensor->scaled_height,
                &tensor->width,&tensor->height,&tensor->pad_x,&tensor->pad_y,&output};
            read.pending=true;
            state.pack->launch(self->width*self->height,args);
            amdva::hip_check(hipEventCreateWithFlags(&tensor->ready,hipEventDisableTiming),"Tensor event creation");
            amdva::hip_check(hipEventRecord(tensor->ready,nullptr),"Tensor event record");
            // Keep the VA allocation/import alive until HIP stops reading it.
            amdva::hip_check(hipEventSynchronize(tensor->ready),"HIP preprocessing completion");
            read.pending=false;
            GST_LOG_OBJECT(self,"VPE ns=%" G_GUINT64_FORMAT ", device tensor=%p",after-before,output);
        }
        if(!amdva::add_tensor_meta(buffer,std::move(tensor))) throw std::runtime_error("Cannot attach tensor metadata");
        return GST_FLOW_OK;
    } catch(const std::exception& e) {
        GST_ELEMENT_ERROR(self,STREAM,FAILED,("AMD preprocessing failed"),("%s",e.what())); return GST_FLOW_ERROR;
    }
}
static void finalize(GObject* o) {
    auto* self=reinterpret_cast<AmdPreproc*>(o); delete self->state;
    G_OBJECT_CLASS(amd_preproc_parent_class)->finalize(o);
}
static void amd_preproc_class_init(AmdPreprocClass* klass) {
    auto* object=G_OBJECT_CLASS(klass); object->set_property=set_property; object->get_property=get_property; object->finalize=finalize;
    const auto flags=static_cast<GParamFlags>(G_PARAM_READWRITE|G_PARAM_STATIC_STRINGS|GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(object,P_DEVICE,g_param_spec_int("device","GPU device","HIP device index",0,16,0,flags));
    g_object_class_install_property(object,P_WIDTH,g_param_spec_uint("tensor-width","Tensor width","Model input width",2,4096,640,flags));
    g_object_class_install_property(object,P_HEIGHT,g_param_spec_uint("tensor-height","Tensor height","Model input height",2,4096,640,flags));
    g_object_class_install_property(object,P_STRICT,g_param_spec_boolean("require-zero-copy","Strict mode","Reject copies and non-VPE scaling",TRUE,flags));
    auto* element=GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element,"AMD VPE/HIP preprocessing","Filter/Video","Preserves VA video and attaches GPU tensor","AMD");
    gst_element_class_add_static_pad_template(element,&sink); gst_element_class_add_static_pad_template(element,&src);
    auto* base=GST_BASE_TRANSFORM_CLASS(klass); base->start=start; base->stop=stop; base->set_caps=set_caps; base->transform_ip=transform;
    GST_DEBUG_CATEGORY_INIT(preproc_debug,"amdpreproc",0,"AMD VPE/HIP preprocessing");
}
static void amd_preproc_init(AmdPreproc* self) {
    self->device=0; self->width=640; self->height=640; self->strict=TRUE;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self),TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self),FALSE);
}
