#include "amdva/tensor_meta.hpp"
#include "amdva/video_preprocess.hpp"
#include <algorithm>
#include <cmath>
#include <gst/base/gstbasetransform.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <gst/video/video.h>

GST_DEBUG_CATEGORY_STATIC(preproc_debug);
#define GST_CAT_DEFAULT preproc_debug
struct PreprocState {
    GstVideoInfo video{};
    std::unique_ptr<amdva::VideoPreprocessor> processor;
};
typedef struct {
    GstBaseTransform parent;
    gint device;
    guint width, height;
    gboolean strict;
    gchar *model_info;
    PreprocState *state;
} AmdPreproc;
typedef struct {
    GstBaseTransformClass parent;
} AmdPreprocClass;
G_DEFINE_TYPE(AmdPreproc, amd_preproc, GST_TYPE_BASE_TRANSFORM)
enum { P_NONE, P_DEVICE, P_WIDTH, P_HEIGHT, P_STRICT, P_INFO };
static GstStaticPadTemplate sink = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]"));
static GstStaticPadTemplate src = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]"));
static void set_property(GObject *o, guint p, const GValue *v, GParamSpec *spec) {
    auto *self = reinterpret_cast<AmdPreproc *>(o);
    if (GST_STATE(self) > GST_STATE_READY) {
        GST_WARNING_OBJECT(self, "Properties can only change in NULL or READY");
        return;
    }
    switch (p) {
    case P_DEVICE:
        self->device = g_value_get_int(v);
        break;
    case P_WIDTH:
        self->width = g_value_get_uint(v);
        break;
    case P_HEIGHT:
        self->height = g_value_get_uint(v);
        break;
    case P_STRICT:
        self->strict = g_value_get_boolean(v);
        break;
    case P_INFO:
        g_free(self->model_info);
        self->model_info = g_value_dup_string(v);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
    }
}
static void get_property(GObject *o, guint p, GValue *v, GParamSpec *spec) {
    auto *self = reinterpret_cast<AmdPreproc *>(o);
    switch (p) {
    case P_DEVICE:
        g_value_set_int(v, self->device);
        break;
    case P_WIDTH:
        g_value_set_uint(v, self->width);
        break;
    case P_HEIGHT:
        g_value_set_uint(v, self->height);
        break;
    case P_STRICT:
        g_value_set_boolean(v, self->strict);
        break;
    case P_INFO:
        g_value_set_string(v, self->model_info);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
    }
}
static gboolean start(GstBaseTransform *t) {
    auto *self = reinterpret_cast<AmdPreproc *>(t);
    if (!self->strict) {
        GST_ELEMENT_ERROR(self, LIBRARY, SETTINGS, ("Only strict zero-copy mode is implemented"),
                          (nullptr));
        return FALSE;
    }
    try {
        auto state = std::make_unique<PreprocState>();
        amdva::ModelInfo info;
        info.width = self->width;
        info.height = self->height;
        if (self->model_info)
            info = amdva::ModelInfo::load(self->model_info);
        state->processor = std::make_unique<amdva::VideoPreprocessor>(info, self->device);
        delete self->state;
        self->state = state.release();
        return TRUE;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(self, LIBRARY, INIT, ("Cannot initialize preprocessing"),
                          ("%s", e.what()));
        return FALSE;
    }
}
static gboolean stop(GstBaseTransform *t) {
    auto *self = reinterpret_cast<AmdPreproc *>(t);
    delete self->state;
    self->state = nullptr;
    return TRUE;
}
static gboolean set_caps(GstBaseTransform *t, GstCaps *in, GstCaps *) {
    auto *self = reinterpret_cast<AmdPreproc *>(t);
    if (!self->state)
        return FALSE;
    auto &video = self->state->video;
    if (!gst_video_info_from_caps(&video, in) ||
        GST_VIDEO_INFO_FORMAT(&video) != GST_VIDEO_FORMAT_NV12 || video.width % 2 ||
        video.height % 2 || video.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE)
        return FALSE;
    // VPE CSC is explicitly BT.709 limited-range. Reject other declared matrices/ranges.
    if ((video.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN &&
         video.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709) ||
        (video.colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN &&
         video.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235))
        return FALSE;
    return TRUE;
}
static GstFlowReturn transform(GstBaseTransform *t, GstBuffer *buffer) {
    auto *self = reinterpret_cast<AmdPreproc *>(t);
    try {
        if (amdva::get_tensor_meta(buffer))
            throw std::runtime_error("Input already carries an AMD tensor");
        auto tensor = self->state->processor->prepare(buffer, self->state->video);
        if (!amdva::add_tensor_meta(buffer, std::move(tensor)))
            throw std::runtime_error("Cannot attach tensor metadata");
        return GST_FLOW_OK;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED, ("AMD preprocessing failed"), ("%s", e.what()));
        return GST_FLOW_ERROR;
    }
}
static void finalize(GObject *o) {
    auto *self = reinterpret_cast<AmdPreproc *>(o);
    delete self->state;
    g_free(self->model_info);
    G_OBJECT_CLASS(amd_preproc_parent_class)->finalize(o);
}
static void amd_preproc_class_init(AmdPreprocClass *klass) {
    auto *object = G_OBJECT_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    const auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                                GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(
        object, P_DEVICE,
        g_param_spec_int("device", "GPU device", "HIP device index", 0, 16, 0, flags));
    g_object_class_install_property(object, P_WIDTH,
                                    g_param_spec_uint("tensor-width", "Tensor width",
                                                      "Model input width", 2, 4096, 640, flags));
    g_object_class_install_property(object, P_HEIGHT,
                                    g_param_spec_uint("tensor-height", "Tensor height",
                                                      "Model input height", 2, 4096, 640, flags));
    g_object_class_install_property(object, P_STRICT,
                                    g_param_spec_boolean("require-zero-copy", "Strict mode",
                                                         "Reject copies and non-VPE scaling", TRUE,
                                                         flags));
    g_object_class_install_property(object, P_INFO,
                                    g_param_spec_string("model-info", "Model info",
                                                        "Model preprocessing JSON", nullptr,
                                                        flags));
    auto *element = GST_ELEMENT_CLASS(klass);
    gst_element_class_set_static_metadata(element, "AMD VPE/HIP preprocessing", "Filter/Video",
                                          "Preserves VA video and attaches GPU tensor", "AMD");
    gst_element_class_add_static_pad_template(element, &sink);
    gst_element_class_add_static_pad_template(element, &src);
    auto *base = GST_BASE_TRANSFORM_CLASS(klass);
    base->start = start;
    base->stop = stop;
    base->set_caps = set_caps;
    base->transform_ip = transform;
    GST_DEBUG_CATEGORY_INIT(preproc_debug, "amdpreproc", 0, "AMD VPE/HIP preprocessing");
}
static void amd_preproc_init(AmdPreproc *self) {
    self->device = 0;
    self->width = 640;
    self->height = 640;
    self->strict = TRUE;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
}
