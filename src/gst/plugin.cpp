#include <gst/gst.h>
GType amd_preproc_get_type();
GType amd_migraphx_get_type();
GType amd_detect_get_type();
GType amd_classify_get_type();
GType amd_inference_get_type();
GType amd_watermark_get_type();
GType amd_python_get_type();
GType amd_meta_publish_get_type();
GType amd_encode_get_type();
static gboolean plugin_init(GstPlugin* p) {
    return gst_element_register(p,"amdpreproc",GST_RANK_NONE,amd_preproc_get_type()) &&
           gst_element_register(p,"amdmigraphx",GST_RANK_NONE,amd_migraphx_get_type()) &&
           gst_element_register(p,"amddetect",GST_RANK_NONE,amd_detect_get_type()) &&
           gst_element_register(p,"amdclassify",GST_RANK_NONE,amd_classify_get_type()) &&
           gst_element_register(p,"amdinference",GST_RANK_NONE,amd_inference_get_type()) &&
           gst_element_register(p,"amdwatermark",GST_RANK_NONE,amd_watermark_get_type()) &&
           gst_element_register(p,"amdpython",GST_RANK_NONE,amd_python_get_type()) &&
           gst_element_register(p,"amdmetapublish",GST_RANK_NONE,amd_meta_publish_get_type()) &&
           gst_element_register(p,"amdencode",GST_RANK_NONE,amd_encode_get_type());
}
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,GST_VERSION_MINOR,amdanalytics,
    "AMD VA/VPE and HIP/MIGraphX analytics",plugin_init,"0.1.0","MIT/X11",PACKAGE,
    "https://github.com/ROCm/AMDMIGraphX")
