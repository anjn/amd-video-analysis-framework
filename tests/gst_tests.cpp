#include "amdva/tensor_meta.hpp"
#include <gst/gst.h>
#include <iostream>
static void require(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
int main(int argc,char** argv) {
    gst_init(&argc,&argv);
    auto* buffer=gst_buffer_new_allocate(nullptr,32,nullptr);GST_BUFFER_PTS(buffer)=7*GST_SECOND;
    auto t=std::make_shared<amdva::Tensor>();auto weak=std::weak_ptr<amdva::Tensor>(t);
    require(amdva::add_tensor_meta(buffer,t)!=nullptr,"Tensor attach failed");t.reset();
    auto* clone=gst_buffer_copy(buffer);
    require(amdva::get_tensor_meta(clone)!=nullptr,"Shallow-copy metadata missing");
    require(GST_BUFFER_PTS(clone)==7*GST_SECOND,"PTS changed");
    require(gst_buffer_peek_memory(buffer,0)==gst_buffer_peek_memory(clone,0),"Payload copied");
    gst_buffer_unref(buffer);require(!weak.expired(),"Tensor freed before clone");
    gst_buffer_unref(clone);require(weak.expired(),"Tensor leaked");
    auto* pre=gst_element_factory_make("amdpreproc",nullptr);require(pre,"amdpreproc unavailable");
    gboolean strict=FALSE;g_object_get(pre,"require-zero-copy",&strict,nullptr);require(strict,"Strict mode not default");
    auto* pad=gst_element_get_static_pad(pre,"sink");auto* caps=gst_pad_query_caps(pad,nullptr);
    auto* host=gst_caps_from_string("video/x-raw,format=NV12,width=1920,height=1080");
    require(!gst_caps_can_intersect(caps,host),"Host memory accepted");gst_caps_unref(caps);gst_caps_unref(host);gst_object_unref(pad);
    g_object_set(pre,"require-zero-copy",FALSE,nullptr);
    require(gst_element_set_state(pre,GST_STATE_PAUSED)==GST_STATE_CHANGE_FAILURE,"Non-strict mode accepted");
    gst_element_set_state(pre,GST_STATE_NULL);gst_object_unref(pre);
    auto* infer=gst_element_factory_make("amdmigraphx",nullptr);require(infer,"amdmigraphx unavailable");
    require(gst_element_set_state(infer,GST_STATE_PAUSED)==GST_STATE_CHANGE_FAILURE,"Missing model accepted");
    gst_element_set_state(infer,GST_STATE_NULL);gst_object_unref(infer);
    std::cout<<"Metadata lifetime, PTS, payload identity, strict caps and initialization rejection passed\n";
}
