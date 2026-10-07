#include "amdva/tensor_meta.hpp"
#include "amdva/video_preprocess.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay_drm.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
static void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main(int argc,char** argv) {
    gst_init(&argc,&argv);
    GstElement* pipeline=nullptr;GstVaDisplay* display=nullptr;GstAllocator* allocator=nullptr;
    try {
        amdva::gpu_arch(0);
        display=gst_va_display_drm_new_from_path("/dev/dri/renderD128");require(display,"Cannot initialize VA display");
        auto* formats=g_array_new(FALSE,FALSE,sizeof(guint));guint format=GST_VIDEO_FORMAT_NV12;g_array_append_val(formats,format);
        allocator=gst_va_allocator_new(display,formats);require(allocator,"VA allocator creation failed");
        GstVideoInfo info;gst_video_info_init(&info);gst_video_info_set_format(&info,GST_VIDEO_FORMAT_NV12,1920,1080);
        info.fps_n=30;info.fps_d=1;gst_video_colorimetry_from_string(&info.colorimetry,"bt709");
        require(gst_va_allocator_set_format(allocator,&info,VA_SURFACE_ATTRIB_USAGE_HINT_DECODER,GST_VA_FEATURE_DISABLED),"VA allocator format failed");
        GError* error=nullptr;pipeline=gst_parse_launch("appsrc name=input format=time ! amdpreproc ! appsink name=output sync=false",&error);
        if(error) {std::string message=error->message;g_clear_error(&error);throw std::runtime_error(message);}
        auto* source=gst_bin_get_by_name(GST_BIN(pipeline),"input");auto* sink=gst_bin_get_by_name(GST_BIN(pipeline),"output");
        auto* caps=gst_video_info_to_caps(&info);
        gst_caps_set_features(caps,0,gst_caps_features_new("memory:VAMemory",nullptr));
        gst_app_src_set_caps(GST_APP_SRC(source),caps);gst_caps_unref(caps);
        require(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"Pipeline start failed");
        std::shared_ptr<amdva::Tensor> first_tensor;
        std::vector<float> first_values;
        amdva::ModelInfo roi_model;roi_model.width=roi_model.height=224;roi_model.letterbox=false;
        roi_model.normalization.bgr=true;roi_model.normalization.mean={.1f,.2f,.3f};roi_model.normalization.stddev={.5f,.6f,.7f};
        amdva::VideoPreprocessor roi_preprocessor(roi_model);
        for(unsigned frame=0;frame<2;frame++) {
        auto* memory=gst_va_allocator_alloc(allocator);require(memory,"VA allocation failed");
        auto* buffer=gst_buffer_new();gst_buffer_append_memory(buffer,memory);
        GST_BUFFER_PTS(buffer)=(123+frame*33)*GST_MSECOND;GST_BUFFER_DURATION(buffer)=GST_SECOND/30;
        auto* original=gst_buffer_peek_memory(buffer,0);
        auto dpy=static_cast<VADisplay>(gst_va_display_get_va_dpy(display));
        const auto surface=gst_va_buffer_get_surface(buffer);
        VAImageFormat image_format{};image_format.fourcc=VA_FOURCC_NV12;image_format.byte_order=VA_LSB_FIRST;
        VAImage image{};amdva::va_check(vaCreateImage(dpy,&image_format,1920,1080,&image),"Fixture image");
        void* host{};amdva::va_check(vaMapBuffer(dpy,image.buf,&host),"Fixture mapping");
        for(unsigned y=0;y<1080;y++)std::memset(static_cast<char*>(host)+image.offsets[0]+y*image.pitches[0],frame?192:64,1920);
        for(unsigned y=0;y<540;y++)for(unsigned x=0;x<1920;x+=2) {
            auto* uv=static_cast<unsigned char*>(host)+image.offsets[1]+y*image.pitches[1]+x;uv[0]=100;uv[1]=180;
        }
        amdva::va_check(vaUnmapBuffer(dpy,image.buf),"Fixture unmap");
        amdva::va_check(vaPutImage(dpy,surface,image.image_id,0,0,1920,1080,0,0,1920,1080),"Fixture upload");
        vaDestroyImage(dpy,image.image_id);
        require(gst_app_src_push_buffer(GST_APP_SRC(source),buffer)==GST_FLOW_OK,"Fixture push failed");
        auto* sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),10*GST_SECOND);require(sample,"No preprocessed sample");
        auto* output=gst_sample_get_buffer(sample);require(gst_buffer_peek_memory(output,0)==original,"Image payload changed");
        require(GST_BUFFER_PTS(output)==(123+frame*33)*GST_MSECOND,"PTS changed");
        auto* meta=amdva::get_tensor_meta(output);require(meta && meta->tensor,"Tensor missing");
        const auto tensor=*meta->tensor;
        require(tensor->scaled_width==640 && tensor->scaled_height==360 && tensor->pad_x==0 && tensor->pad_y==140,"Letterbox geometry wrong");
        std::vector<float> values(3*640*640);
        amdva::hip_check(hipMemcpy(values.data(),tensor->buffer->data(),values.size()*sizeof(float),hipMemcpyDeviceToHost),"Verification-only tensor download");
        // Independent BT.709 limited-range reference for two distinct fixtures.
        const float y=((frame?192.0f:64.0f)-16)*255/219;
        const float u=(100.0f-128)*255/224,v=(180.0f-128)*255/224;
        const float expected[]={std::clamp((y+1.5748f*v)/255,0.0f,1.0f),
            std::clamp((y-0.187324f*u-0.468124f*v)/255,0.0f,1.0f),
            std::clamp((y+1.8556f*u)/255,0.0f,1.0f)};
        for(unsigned c=0;c<3;c++) {
            require(std::abs(values[c*640*640]-114.0f/255)<1e-6,"Padding value incorrect");
            require(std::abs(values[c*640*640+140*640+320]-expected[c])<2.0f/255,"RGB channel/normalization incorrect");
            require(std::abs(values[c*640*640+499*640+639]-expected[c])<2.0f/255,"Pitch or bottom edge incorrect");
            require(std::abs(values[c*640*640+500*640]-114.0f/255)<1e-6,"Bottom padding incorrect");
        }
        auto cropped=roi_preprocessor.prepare(output,info,11,233,1897,513);
        require(cropped->geometry.crop_x==10&&cropped->geometry.crop_y==232&&cropped->geometry.crop_width==1898&&cropped->geometry.crop_height==514,"NV12 ROI alignment");
        std::vector<float> roi_values(3*224*224);amdva::hip_check(hipMemcpy(roi_values.data(),cropped->buffer->data(),roi_values.size()*sizeof(float),hipMemcpyDeviceToHost),"Offline ROI reference");
        for(unsigned c=0;c<3;c++){
            const float normalized=(expected[2-c]-roi_model.normalization.mean[c])/roi_model.normalization.stddev[c];
            require(std::abs(roi_values[c*224*224+112*224+112]-normalized)<.04f,"Multistage ROI color/BGR/mean/std reference");
        }
        if(!frame) {first_tensor=tensor;first_values=values;}
        else {
            require(first_tensor->buffer->data()!=tensor->buffer->data(),"Live tensor allocation reused");
            amdva::hip_check(hipMemcpy(values.data(),first_tensor->buffer->data(),values.size()*sizeof(float),hipMemcpyDeviceToHost),"Previous tensor verification");
            require(values==first_values,"Previous tensor overwritten by surface reuse");
        }
        gst_sample_unref(sample);
        }
        gst_app_src_end_of_stream(GST_APP_SRC(source));
        gst_object_unref(source);gst_object_unref(sink);
        gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);pipeline=nullptr;
        gst_object_unref(allocator);allocator=nullptr;gst_object_unref(display);display=nullptr;
        std::cout<<"{\"passed\":true,\"rgb_bt709\":true,\"letterbox\":true,\"payload_identity\":true,\"pts\":true,\"frames\":2,\"surface_reuse\":true,\"live_tensor_preserved\":true}\n";return 0;
    } catch(const std::exception& e) {
        if(pipeline){gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline);}
        if(allocator)gst_object_unref(allocator);if(display)gst_object_unref(display);
        std::cerr<<"Preprocessing reference failed: "<<e.what()<<"\n";return 2;
    }
}
