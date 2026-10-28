// amdencode: VA hardware video encoder bin. Accepts VAMemory or DMA-BUF frames
// (for example the BGRA DMA-BUF output of amdwatermark), converts to NV12 VAMemory
// with the VA video processor and encodes with the VA H.264/H.265/AV1 encoder.
// No CPU pixel transfer is added; encoded output is byte-stream, AU aligned.
#include "amdva/vui.hpp"
#include <exception>
#include <gst/gst.h>
#include <initializer_list>
#include <gst/va/gstvautils.h>


// ---- DMA-BUF BGRA -> NV12 VAMemory import stage ------------------------------
// amdwatermark emits linear BGRA DMA-BUF frames that VA elements cannot negotiate.
// This private stage imports the DMA-BUF as a VA surface (no copy) and converts
// it to a pooled NV12 VAMemory frame on the VA video processor. VAMemory input
// passes through untouched.
#include "amdva/runtime.hpp"
#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/base/gstbasetransform.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvapool.h>
#include <gst/video/video.h>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>

typedef struct {
    GstBaseTransform parent;
    GstVaDisplay *display;
    GstBufferPool *pool;
    VAConfigID config;
    VAContextID context;
    std::map<std::pair<dev_t, ino_t>, VASurfaceID> *imported;
    GstVideoInfo info;
    gboolean convert;
} AmdVaImport;
typedef struct {
    GstBaseTransformClass parent;
} AmdVaImportClass;
G_DEFINE_TYPE(AmdVaImport, amd_va_import, GST_TYPE_BASE_TRANSFORM)
static GstStaticPadTemplate import_sink = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:DMABuf),format={BGRA,BGRx};"
                    "video/x-raw(memory:VAMemory),format={NV12,BGRA,BGRx,RGBA}"));
static GstStaticPadTemplate import_src = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format={NV12,BGRA,BGRx,RGBA}"));

static void import_release(AmdVaImport *s) {
    auto *va = s->display ? static_cast<VADisplay>(gst_va_display_get_va_dpy(s->display)) : nullptr;
    if (s->imported && va)
        for (auto &item : *s->imported)
            vaDestroySurfaces(va, &item.second, 1);
    if (s->imported)
        s->imported->clear();
    if (s->pool) {
        gst_buffer_pool_set_active(s->pool, FALSE);
        gst_object_unref(s->pool);
        s->pool = nullptr;
    }
    if (va && s->context != VA_INVALID_ID)
        vaDestroyContext(va, s->context);
    if (va && s->config != VA_INVALID_ID)
        vaDestroyConfig(va, s->config);
    s->context = s->config = VA_INVALID_ID;
}
static void import_set_context(GstElement *e, GstContext *c) {
    auto *s = reinterpret_cast<AmdVaImport *>(e);
    gst_va_handle_set_context(e, c, nullptr, &s->display);
    GST_ELEMENT_CLASS(amd_va_import_parent_class)->set_context(e, c);
}
static gboolean import_query(GstBaseTransform *t, GstPadDirection d, GstQuery *q) {
    auto *s = reinterpret_cast<AmdVaImport *>(t);
    if (GST_QUERY_TYPE(q) == GST_QUERY_CONTEXT &&
        gst_va_handle_context_query(GST_ELEMENT(t), q, s->display))
        return TRUE;
    return GST_BASE_TRANSFORM_CLASS(amd_va_import_parent_class)->query(t, d, q);
}
static GstCaps *import_transform_caps(GstBaseTransform *, GstPadDirection direction,
                                      GstCaps *caps, GstCaps *filter) {
    auto *result = gst_caps_new_empty();
    for (guint i = 0; i < gst_caps_get_size(caps); i++) {
        auto *structure = gst_caps_get_structure(caps, i);
        auto *features = gst_caps_get_features(caps, i);
        const bool dmabuf = gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_DMABUF);
        if (direction == GST_PAD_SINK) {
            auto *out = gst_structure_copy(structure);
            if (dmabuf)
                gst_structure_set(out, "format", G_TYPE_STRING, "NV12", nullptr);
            gst_structure_remove_fields(out, "colorimetry", "chroma-site", nullptr);
            gst_caps_append_structure_full(
                result, out, gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_VA, nullptr));
        } else {
            auto *same = gst_structure_copy(structure);
            gst_caps_append_structure_full(
                result, same, gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_VA, nullptr));
            auto *format = gst_structure_get_string(structure, "format");
            if (!format || !g_strcmp0(format, "NV12")) {
                auto *out = gst_structure_copy(structure);
                gst_structure_set(out, "format", G_TYPE_STRING, "BGRA", nullptr);
                gst_structure_remove_fields(out, "colorimetry", "chroma-site", nullptr);
                gst_caps_append_structure_full(
                    result, out, gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_DMABUF, nullptr));
            }
        }
    }
    if (filter) {
        auto *intersection = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(result);
        result = intersection;
    }
    return result;
}
static gboolean import_set_caps(GstBaseTransform *t, GstCaps *in, GstCaps *out) {
    auto *s = reinterpret_cast<AmdVaImport *>(t);
    import_release(s);
    if (!gst_video_info_from_caps(&s->info, in)) {
        GST_ELEMENT_ERROR(t, CORE, NEGOTIATION, ("Invalid input caps"), (nullptr));
        return FALSE;
    }
    s->convert = gst_caps_features_contains(gst_caps_get_features(in, 0),
                                            GST_CAPS_FEATURE_MEMORY_DMABUF);
    gst_base_transform_set_passthrough(t, !s->convert);
    if (!s->convert)
        return TRUE;
    // An upstream VA element normally answers the context query; otherwise open the
    // first render node.
    std::string node = "/dev/dri/renderD128";
    for (int i = 128; i < 192; i++) {
        auto candidate = "/dev/dri/renderD" + std::to_string(i);
        if (g_file_test(candidate.c_str(), G_FILE_TEST_EXISTS)) {
            node = candidate;
            break;
        }
    }
    if (!gst_va_ensure_element_data(t, node.c_str(), &s->display)) {
        GST_ELEMENT_ERROR(t, RESOURCE, NOT_FOUND, ("No VA display"), (nullptr));
        return FALSE;
    }
    auto *va = static_cast<VADisplay>(gst_va_display_get_va_dpy(s->display));
    try {
        amdva::va_check(vaCreateConfig(va, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                                       &s->config),
                        "VA import config");
        amdva::va_check(vaCreateContext(va, s->config, 0, 0, 0, nullptr, 0, &s->context),
                        "VA import context");
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(t, RESOURCE, FAILED, ("VA video processor unavailable"), ("%s", e.what()));
        import_release(s);
        return FALSE;
    }
    auto *formats = g_array_new(FALSE, FALSE, sizeof(GstVideoFormat));
    GstVideoFormat nv12 = GST_VIDEO_FORMAT_NV12;
    g_array_append_val(formats, nv12);
    auto *allocator = gst_va_allocator_new(s->display, formats);
    s->pool = gst_va_pool_new_with_config(out, 4, 8, VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE,
                                          GST_VA_FEATURE_DISABLED, allocator, nullptr);
    gst_object_unref(allocator);
    if (!s->pool || !gst_buffer_pool_set_active(s->pool, TRUE)) {
        GST_ELEMENT_ERROR(t, RESOURCE, FAILED, ("Cannot allocate NV12 VA pool"), (nullptr));
        import_release(s);
        return FALSE;
    }
    return TRUE;
}
static gboolean import_start(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdVaImport *>(t);
    s->imported = new std::map<std::pair<dev_t, ino_t>, VASurfaceID>();
    s->config = s->context = VA_INVALID_ID;
    return TRUE;
}
static gboolean import_stop(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdVaImport *>(t);
    import_release(s);
    delete s->imported;
    s->imported = nullptr;
    gst_clear_object(&s->display);
    return TRUE;
}
static gboolean import_decide(GstBaseTransform *, GstQuery *) { return TRUE; }
static GstFlowReturn import_prepare(GstBaseTransform *t, GstBuffer *in, GstBuffer **out) {
    auto *s = reinterpret_cast<AmdVaImport *>(t);
    if (!s->convert)
        return GST_BASE_TRANSFORM_CLASS(amd_va_import_parent_class)->prepare_output_buffer(t, in, out);
    GstBufferPoolAcquireParams params{};
    return gst_buffer_pool_acquire_buffer(s->pool, out, &params);
}
static VASurfaceID import_surface(AmdVaImport *s, GstBuffer *buffer) {
    auto *memory = gst_buffer_peek_memory(buffer, 0);
    if (!memory || !gst_is_dmabuf_memory(memory))
        throw std::runtime_error("Input is not DMA-BUF");
    const int fd = gst_dmabuf_memory_get_fd(memory);
    struct stat st{};
    if (fstat(fd, &st))
        throw std::runtime_error("Cannot identify DMA-BUF");
    const auto key = std::make_pair(st.st_dev, st.st_ino);
    if (auto found = s->imported->find(key); found != s->imported->end())
        return found->second;
    auto *meta = gst_buffer_get_video_meta(buffer);
    const gsize offset = meta ? meta->offset[0] : 0;
    const guint pitch = meta ? meta->stride[0] : GST_VIDEO_INFO_PLANE_STRIDE(&s->info, 0);
    VADRMPRIMESurfaceDescriptor descriptor{};
    descriptor.fourcc = VA_FOURCC_BGRA;
    descriptor.width = GST_VIDEO_INFO_WIDTH(&s->info);
    descriptor.height = GST_VIDEO_INFO_HEIGHT(&s->info);
    descriptor.num_objects = 1;
    descriptor.objects[0].fd = fd;
    descriptor.objects[0].size = gst_memory_get_sizes(memory, nullptr, nullptr);
    descriptor.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    descriptor.num_layers = 1;
    descriptor.layers[0].drm_format = DRM_FORMAT_ARGB8888;
    descriptor.layers[0].num_planes = 1;
    descriptor.layers[0].object_index[0] = 0;
    descriptor.layers[0].offset[0] = offset;
    descriptor.layers[0].pitch[0] = pitch;
    VASurfaceAttrib attrs[2]{};
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    attrs[1].value.value.p = &descriptor;
    auto *va = static_cast<VADisplay>(gst_va_display_get_va_dpy(s->display));
    VASurfaceID surface = VA_INVALID_SURFACE;
    amdva::va_check(vaCreateSurfaces(va, VA_RT_FORMAT_RGB32, descriptor.width, descriptor.height,
                                     &surface, 1, attrs, 2),
                    "VA DMA-BUF BGRA import");
    (*s->imported)[key] = surface;
    return surface;
}
static GstFlowReturn import_transform(GstBaseTransform *t, GstBuffer *in, GstBuffer *out) {
    auto *s = reinterpret_cast<AmdVaImport *>(t);
    auto *va = static_cast<VADisplay>(gst_va_display_get_va_dpy(s->display));
    VABufferID params = VA_INVALID_ID;
    gst_buffer_copy_into(out, in, GstBufferCopyFlags(GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS),
                         0, -1);
    try {
        const auto source = import_surface(s, in);
        const auto dst = gst_va_buffer_get_surface(out);
        const auto width = GST_VIDEO_INFO_WIDTH(&s->info), height = GST_VIDEO_INFO_HEIGHT(&s->info);
        VARectangle region{0, 0, uint16_t(width), uint16_t(height)};
        VAProcPipelineParameterBuffer pp{};
        pp.surface = source;
        pp.surface_region = &region;
        pp.output_region = &region;
        pp.surface_color_standard = VAProcColorStandardBT709;
        pp.output_color_standard = VAProcColorStandardBT709;
        pp.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        pp.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
        amdva::va_check(vaCreateBuffer(va, s->context, VAProcPipelineParameterBufferType,
                                       sizeof(pp), 1, &pp, &params),
                        "VA conversion parameters");
        amdva::va_check(vaBeginPicture(va, s->context, dst), "VA conversion begin");
        amdva::va_check(vaRenderPicture(va, s->context, &params, 1), "VA conversion render");
        amdva::va_check(vaEndPicture(va, s->context), "VA conversion end");
        amdva::va_check(vaSyncSurface(va, dst), "VA conversion completion");
        vaDestroyBuffer(va, params);
    } catch (const std::exception &e) {
        if (params != VA_INVALID_ID)
            vaDestroyBuffer(va, params);
        GST_ELEMENT_ERROR(t, STREAM, ENCODE, ("BGRA to NV12 conversion failed"), ("%s", e.what()));
        return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
}
static void amd_va_import_class_init(AmdVaImportClass *k) {
    auto *element = GST_ELEMENT_CLASS(k);
    element->set_context = import_set_context;
    gst_element_class_set_static_metadata(element, "AMD VA DMA-BUF import", "Filter/Converter/Video",
                                          "Imports BGRA DMA-BUF frames as NV12 VAMemory", "AMD");
    gst_element_class_add_static_pad_template(element, &import_sink);
    gst_element_class_add_static_pad_template(element, &import_src);
    auto *base = GST_BASE_TRANSFORM_CLASS(k);
    base->transform_caps = import_transform_caps;
    base->set_caps = import_set_caps;
    base->start = import_start;
    base->stop = import_stop;
    base->query = import_query;
    base->decide_allocation = import_decide;
    base->prepare_output_buffer = import_prepare;
    base->transform = import_transform;
    base->passthrough_on_same_caps = TRUE;
}
static void amd_va_import_init(AmdVaImport *s) {
    s->config = s->context = VA_INVALID_ID;
}
// ---- amdencode bin ------------------------------------------------------------
typedef struct {
    GstBin parent;
    gint codec, rate_control;
    guint bitrate, key_int_max, b_frames;
    gboolean signal_bt709;
    GstPad *sink, *src;
    GstElement *encoder;
    gulong vui_probe;
} AmdEncode;
typedef struct {
    GstBinClass parent;
} AmdEncodeClass;
G_DEFINE_TYPE(AmdEncode, amd_encode, GST_TYPE_BIN)
enum { P_NONE, P_CODEC, P_BITRATE, P_KEY_INT_MAX, P_B_FRAMES, P_SIGNAL_BT709, P_RATE_CONTROL };
enum { CODEC_H264, CODEC_H265, CODEC_AV1 };
enum { RATE_CBR, RATE_VBR, RATE_QVBR };
static GType rate_control_type() {
    static GType type;
    if (g_once_init_enter(&type)) {
        static const GEnumValue values[] = {
            {RATE_CBR, "Constant bitrate", "cbr"},
            {RATE_VBR, "Variable bitrate up to the target", "vbr"},
            {RATE_QVBR, "Quality-defined variable bitrate", "qvbr"},
            {0, nullptr, nullptr}};
        g_once_init_leave(&type, g_enum_register_static("AmdEncodeRateControl", values));
    }
    return type;
}
static GType codec_type() {
    static GType type;
    if (g_once_init_enter(&type)) {
        static const GEnumValue values[] = {
            {CODEC_H264, "H.264/AVC", "h264"}, {CODEC_H265, "H.265/HEVC", "h265"},
            {CODEC_AV1, "AV1", "av1"}, {0, nullptr, nullptr}};
        g_once_init_leave(&type, g_enum_register_static("AmdEncodeCodec", values));
    }
    return type;
}
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format={NV12,BGRA,BGRx,RGBA};"
                    "video/x-raw(memory:DMABuf),format={NV12,BGRA,BGRx,RGBA}"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-h264,stream-format=byte-stream,alignment=au;"
                    "video/x-h265,stream-format=byte-stream,alignment=au;"
                    "video/x-av1,stream-format=obu-stream,alignment=tu"));

static void set_property(GObject *o, guint p, const GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdEncode *>(o);
    if (GST_STATE(s) > GST_STATE_READY)
        return;
    switch (p) {
    case P_CODEC: s->codec = g_value_get_enum(v); break;
    case P_BITRATE: s->bitrate = g_value_get_uint(v); break;
    case P_KEY_INT_MAX: s->key_int_max = g_value_get_uint(v); break;
    case P_B_FRAMES: s->b_frames = g_value_get_uint(v); break;
    case P_SIGNAL_BT709: s->signal_bt709 = g_value_get_boolean(v); break;
    case P_RATE_CONTROL: s->rate_control = g_value_get_enum(v); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
    }
}
static void get_property(GObject *o, guint p, GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdEncode *>(o);
    switch (p) {
    case P_CODEC: g_value_set_enum(v, s->codec); break;
    case P_BITRATE: g_value_set_uint(v, s->bitrate); break;
    case P_KEY_INT_MAX: g_value_set_uint(v, s->key_int_max); break;
    case P_B_FRAMES: g_value_set_uint(v, s->b_frames); break;
    case P_SIGNAL_BT709: g_value_set_boolean(v, s->signal_bt709); break;
    case P_RATE_CONTROL: g_value_set_enum(v, s->rate_control); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
    }
}
static GstPadProbeReturn vui_probe(GstPad *, GstPadProbeInfo *info, gpointer data) {
    auto *original = GST_PAD_PROBE_INFO_BUFFER(info);
    try {
        auto *updated = amdva::signal_bt709(original);
        GST_PAD_PROBE_INFO_DATA(info) = updated;
        gst_buffer_unref(original);
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(GST_ELEMENT(data), STREAM, ENCODE, ("BT.709 signalling failed"),
                          ("%s", e.what()));
        return GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
}
static void clear(AmdEncode *s) {
    if (s->encoder && s->vui_probe) {
        if (auto *pad = gst_element_get_static_pad(s->encoder, "src")) {
            gst_pad_remove_probe(pad, s->vui_probe);
            gst_object_unref(pad);
        }
    }
    s->vui_probe = 0;
    s->encoder = nullptr;
    gst_ghost_pad_set_target(GST_GHOST_PAD(s->sink), nullptr);
    gst_ghost_pad_set_target(GST_GHOST_PAD(s->src), nullptr);
    while (GST_BIN_CHILDREN(s)) {
        auto *child = GST_ELEMENT(GST_BIN_CHILDREN(s)->data);
        gst_object_ref(child);
        gst_bin_remove(GST_BIN(s), child);
        gst_element_set_state(child, GST_STATE_NULL);
        gst_object_unref(child);
    }
}
static bool build(AmdEncode *s) {
    static const char *encoders[] = {"vah264enc", "vah265enc", "vaav1enc"};
    static const char *parsers[] = {"h264parse", "h265parse", "av1parse"};
    auto *import = GST_ELEMENT(g_object_new(amd_va_import_get_type(), nullptr));
    auto *post = gst_element_factory_make("vapostproc", nullptr);
    auto *caps = gst_element_factory_make("capsfilter", nullptr);
    auto *enc = gst_element_factory_make(encoders[s->codec], "encoder");
    auto *parse = gst_element_factory_make(parsers[s->codec], nullptr);
    if (!import || !post || !caps || !enc || !parse) {
        GST_ELEMENT_ERROR(s, CORE, MISSING_PLUGIN, ("Hardware encoder unavailable"),
                          ("Need vapostproc, %s and %s; no software fallback is provided",
                           encoders[s->codec], parsers[s->codec]));
        for (auto *e : {import, post, caps, enc, parse})
            if (e)
                gst_object_unref(e);
        return false;
    }
    // Import drops the colorimetry; without it the VA encoders label the stream BT.601
    // in caps and in the H.265/AV1 headers, which BT.709 players reject.
    auto *nv12 = gst_caps_from_string(
        s->signal_bt709 ? "video/x-raw(memory:VAMemory),format=NV12,colorimetry=bt709"
                        : "video/x-raw(memory:VAMemory),format=NV12");
    g_object_set(caps, "caps", nv12, nullptr);
    gst_caps_unref(nv12);
    g_object_set(enc, "bitrate", s->bitrate, "key-int-max", s->key_int_max, nullptr);
    // The VA encoders list only the rate-control modes the driver supports.
    static const char *modes[] = {"cbr", "vbr", "qvbr"};
    auto *mode = g_object_class_find_property(G_OBJECT_GET_CLASS(enc), "rate-control");
    if (!mode || !G_IS_PARAM_SPEC_ENUM(mode) ||
        !g_enum_get_value_by_nick(G_PARAM_SPEC_ENUM(mode)->enum_class, modes[s->rate_control])) {
        GST_ELEMENT_ERROR(s, CORE, NOT_IMPLEMENTED, ("Unsupported rate control"),
                          ("%s does not offer %s", encoders[s->codec], modes[s->rate_control]));
        for (auto *e : {import, post, caps, enc, parse})
            gst_object_unref(e);
        return false;
    }
    gst_util_set_object_arg(G_OBJECT(enc), "rate-control", modes[s->rate_control]);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(enc), "b-frames"))
        g_object_set(enc, "b-frames", s->b_frames, nullptr);
    gst_bin_add_many(GST_BIN(s), import, post, caps, enc, parse, nullptr);
    if (!gst_element_link_many(import, post, caps, enc, parse, nullptr)) {
        GST_ELEMENT_ERROR(s, CORE, NEGOTIATION, ("Cannot link encoder chain"), (nullptr));
        return false;
    }
    auto *in = gst_element_get_static_pad(import, "sink");
    auto *out = gst_element_get_static_pad(parse, "src");
    gst_ghost_pad_set_target(GST_GHOST_PAD(s->sink), in);
    gst_ghost_pad_set_target(GST_GHOST_PAD(s->src), out);
    gst_object_unref(in);
    gst_object_unref(out);
    s->encoder = enc;
    if (s->codec == CODEC_H264 && s->signal_bt709) {
        auto *pad = gst_element_get_static_pad(enc, "src");
        s->vui_probe = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, vui_probe, s, nullptr);
        gst_object_unref(pad);
    }
    return true;
}
static GstStateChangeReturn change_state(GstElement *e, GstStateChange t) {
    auto *s = reinterpret_cast<AmdEncode *>(e);
    if (t == GST_STATE_CHANGE_NULL_TO_READY && !build(s)) {
        clear(s);
        return GST_STATE_CHANGE_FAILURE;
    }
    auto ret = GST_ELEMENT_CLASS(amd_encode_parent_class)->change_state(e, t);
    if (t == GST_STATE_CHANGE_READY_TO_NULL)
        clear(s);
    return ret;
}
static void amd_encode_class_init(AmdEncodeClass *k) {
    auto *object = G_OBJECT_CLASS(k);
    object->set_property = set_property;
    object->get_property = get_property;
    auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                          GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(
        object, P_CODEC,
        g_param_spec_enum("codec", "Codec", "Output codec", codec_type(), CODEC_H264, flags));
    g_object_class_install_property(
        object, P_BITRATE,
        g_param_spec_uint("bitrate", "Bitrate", "Target bitrate in kbit/s", 1, 2048000, 4000,
                          flags));
    g_object_class_install_property(
        object, P_KEY_INT_MAX,
        g_param_spec_uint("key-int-max", "Key interval", "Maximum frames between keyframes", 1,
                          10000, 30, flags));
    g_object_class_install_property(
        object, P_B_FRAMES,
        g_param_spec_uint("b-frames", "B frames", "Consecutive B frames (0 for low latency)", 0,
                          4, 0, flags));
    g_object_class_install_property(
        object, P_RATE_CONTROL,
        g_param_spec_enum("rate-control", "Rate control",
                          "cbr keeps the bitrate, vbr and qvbr spend fewer bits on easy scenes",
                          rate_control_type(), RATE_CBR, flags));
    g_object_class_install_property(
        object, P_SIGNAL_BT709,
        g_param_spec_boolean("signal-bt709", "Signal BT.709",
                             "Encode and signal BT.709 limited range (caps, and the H.264 SPS "
                             "VUI)",
                             TRUE,
                             flags));
    auto *element = GST_ELEMENT_CLASS(k);
    element->change_state = change_state;
    gst_element_class_set_static_metadata(
        element, "AMD VA hardware encoder", "Codec/Encoder/Video",
        "Encodes VAMemory or DMA-BUF frames on the GPU video engine", "AMD");
    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);
}
static void amd_encode_init(AmdEncode *s) {
    s->codec = CODEC_H264;
    s->rate_control = RATE_CBR;
    s->bitrate = 4000;
    s->key_int_max = 30;
    s->b_frames = 0;
    s->signal_bt709 = TRUE;
    auto *k = GST_ELEMENT_GET_CLASS(s);
    s->sink = gst_ghost_pad_new_no_target_from_template(
        "sink", gst_element_class_get_pad_template(k, "sink"));
    s->src = gst_ghost_pad_new_no_target_from_template(
        "src", gst_element_class_get_pad_template(k, "src"));
    gst_element_add_pad(GST_ELEMENT(s), s->sink);
    gst_element_add_pad(GST_ELEMENT(s), s->src);
}
