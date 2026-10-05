#include "amdva/tensor_meta.hpp"
namespace amdva {
static gboolean init(GstMeta* m,gpointer,GstBuffer*) {
    reinterpret_cast<TensorMeta*>(m)->tensor=nullptr; return TRUE;
}
static void free_meta(GstMeta* m,GstBuffer*) { delete reinterpret_cast<TensorMeta*>(m)->tensor; }
static gboolean transform(GstBuffer* out,GstMeta* m,GstBuffer*,GQuark type,gpointer data) {
    if(!GST_META_TRANSFORM_IS_COPY(type)) return FALSE;
    auto* copy=static_cast<GstMetaTransformCopy*>(data);
    if(copy->region) return FALSE;
    auto* tm=reinterpret_cast<TensorMeta*>(m);
    return tm->tensor && add_tensor_meta(out,*tm->tensor)!=nullptr;
}
GType tensor_meta_api() {
    static gsize type=0;
    if(g_once_init_enter(&type)) {
        const gchar* tags[]={"amd-device-tensor",nullptr};
        auto t=gst_meta_api_type_register("AmdDeviceTensorMetaAPI",tags);
        g_once_init_leave(&type,t);
    }
    return static_cast<GType>(type);
}
const GstMetaInfo* tensor_meta_info() {
    static gsize info=0;
    if(g_once_init_enter(&info)) {
        auto* i=gst_meta_register(tensor_meta_api(),"AmdDeviceTensorMeta",sizeof(TensorMeta),init,free_meta,transform);
        g_once_init_leave(&info,reinterpret_cast<gsize>(i));
    }
    return reinterpret_cast<const GstMetaInfo*>(info);
}
TensorMeta* add_tensor_meta(GstBuffer* b,std::shared_ptr<Tensor> t) {
    auto* m=reinterpret_cast<TensorMeta*>(gst_buffer_add_meta(b,tensor_meta_info(),nullptr));
    if(!m) return nullptr;
    try { m->tensor=new std::shared_ptr<Tensor>(std::move(t)); }
    catch (...) { gst_buffer_remove_meta(b,&m->meta); return nullptr; }
    return m;
}
TensorMeta* get_tensor_meta(GstBuffer* b) {
    return reinterpret_cast<TensorMeta*>(gst_buffer_get_meta(b,tensor_meta_api()));
}
}
