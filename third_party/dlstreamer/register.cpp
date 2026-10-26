#include <gst/gst.h>
#include "gstgvatrack.h"
#include "gstgvametaconvert.h"
#include "config.h"
#include "gstgvastreammux.h"
#include "gstgvastreamdemux.h"
#include "gstgvametaaggregate.h"
#include <dlstreamer/gst/metadata/gva_json_meta.h>
#include <dlstreamer/gst/metadata/gva_tensor_meta.h>
static gboolean initialize(GstPlugin* p) {
    gst_gva_json_meta_get_info();
    gst_gva_tensor_meta_get_info();
    return gst_element_register(p,"gvatrack",GST_RANK_NONE,gst_gva_track_get_type()) &&
        gst_element_register(p,"gvametaconvert",GST_RANK_NONE,gst_gva_meta_convert_get_type()) &&
        gst_element_register(p,"gvastreammux",GST_RANK_NONE,gst_gva_streammux_get_type()) &&
        gst_element_register(p,"gvastreamdemux",GST_RANK_NONE,gst_gva_streamdemux_get_type()) &&
        gst_element_register(p,"gvametaaggregate",GST_RANK_NONE,gst_gva_meta_aggregate_get_type());
}
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,GST_VERSION_MINOR,gva,
    "DL Streamer tracking, metadata conversion and stream mux (vendored subset)",initialize,PLUGIN_VERSION,PLUGIN_LICENSE,PACKAGE,GST_PACKAGE_ORIGIN)
