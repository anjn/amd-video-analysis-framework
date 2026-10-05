#include <gst/gst.h>
GType amd_preproc_get_type();
GType amd_migraphx_get_type();
static gboolean plugin_init(GstPlugin* p) {
    return gst_element_register(p,"amdpreproc",GST_RANK_NONE,amd_preproc_get_type()) &&
           gst_element_register(p,"amdmigraphx",GST_RANK_NONE,amd_migraphx_get_type());
}
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,GST_VERSION_MINOR,amdanalytics,
    "AMD VA/VPE and HIP/MIGraphX analytics",plugin_init,"0.1.0","MIT",PACKAGE,
    "https://github.com/ROCm/AMDMIGraphX")
