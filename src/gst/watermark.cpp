#include "amdva/overlay.hpp"
#include <atomic>
#include <condition_variable>
#include <gst/allocators/gstdmabuf.h>
#include <gst/base/gstbasetransform.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <gst/video/video.h>
#include <unistd.h>
namespace {
struct Display {
    GstVaDisplay *value{};
    explicit Display(GstVaDisplay *d) : value(GST_VA_DISPLAY(gst_object_ref(d))) {}
    ~Display() { gst_object_unref(value); }
};
struct Surface {
    std::shared_ptr<Display> display;
    std::shared_ptr<amdva::VaSession> va;
    VASurfaceID id{VA_INVALID_SURFACE};
    std::unique_ptr<amdva::ImportedSurface> image;
    int fd{-1};
    bool busy{};
    ~Surface() {
        image.reset();
        if (fd >= 0)
            close(fd);
        if (va)
            va->destroy_surface(id);
    }
};
struct Pool {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::shared_ptr<Surface>> slots;
    std::atomic<bool> flushing{false};
};
struct Release {
    std::shared_ptr<Pool> pool;
    std::shared_ptr<Surface> surface;
};
void release(gpointer opaque, GstMiniObject *) {
    std::unique_ptr<Release> token(static_cast<Release *>(opaque));
    {
        std::lock_guard<std::mutex> lock(token->pool->mutex);
        token->surface->busy = false;
    }
    token->pool->ready.notify_one();
}
struct State {
    GstVideoInfo video{};
    int device{};
    std::shared_ptr<Pool> pool = std::make_shared<Pool>();
    std::shared_ptr<Display> display;
    std::shared_ptr<amdva::VaSession> va;
    std::unique_ptr<amdva::OverlayRenderer> renderer;
    GstAllocator *allocator{gst_dmabuf_allocator_new()};
    ~State() {
        pool->flushing = true;
        pool->ready.notify_all();
        gst_object_unref(allocator);
    }
};
} // namespace
typedef struct {
    GstBaseTransform parent;
    gint device;
    gboolean strict;
    State *state;
} AmdWatermark;
typedef struct {
    GstBaseTransformClass parent;
} AmdWatermarkClass;
G_DEFINE_TYPE(AmdWatermark, amd_watermark, GST_TYPE_BASE_TRANSFORM)
enum { P_NONE, P_DEVICE, P_STRICT };
static GstStaticPadTemplate sink = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:VAMemory),format=NV12,width=[2,4096],height=[2,4096]"));
static GstStaticPadTemplate src = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:DMABuf),format=BGRA,width=[2,4096],height=[2,4096]"));
static void set_property(GObject *o, guint p, const GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdWatermark *>(o);
    if (GST_STATE(s) > GST_STATE_READY)
        return;
    if (p == P_DEVICE)
        s->device = g_value_get_int(v);
    else if (p == P_STRICT)
        s->strict = g_value_get_boolean(v);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
}
static void get_property(GObject *o, guint p, GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdWatermark *>(o);
    if (p == P_DEVICE)
        g_value_set_int(v, s->device);
    else if (p == P_STRICT)
        g_value_set_boolean(v, s->strict);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
}
static gboolean start(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdWatermark *>(t);
    try {
        if (!s->strict)
            throw std::runtime_error("Strict GPU mode is required");
        auto state = std::make_unique<State>();
        state->device = s->device;
        delete s->state;
        s->state = state.release();
        return TRUE;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(s, LIBRARY, INIT, ("GPU watermark initialization failed"),
                          ("%s", e.what()));
        return FALSE;
    }
}
static gboolean stop(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdWatermark *>(t);
    delete s->state;
    s->state = nullptr;
    return TRUE;
}
static gboolean event(GstBaseTransform *t, GstEvent *e) {
    auto *s = reinterpret_cast<AmdWatermark *>(t);
    if (s->state) {
        if (GST_EVENT_TYPE(e) == GST_EVENT_FLUSH_START)
            s->state->pool->flushing = true;
        if (GST_EVENT_TYPE(e) == GST_EVENT_FLUSH_STOP)
            s->state->pool->flushing = false;
        s->state->pool->ready.notify_all();
    }
    return GST_BASE_TRANSFORM_CLASS(amd_watermark_parent_class)->sink_event(t, e);
}
static GstCaps *transform_caps(GstBaseTransform *, GstPadDirection direction, GstCaps *caps,
                               GstCaps *filter) {
    auto *result = gst_caps_copy(caps);
    for (unsigned i = 0; i < gst_caps_get_size(result); i++) {
        gst_structure_set(gst_caps_get_structure(result, i), "format", G_TYPE_STRING,
                          direction == GST_PAD_SINK ? "BGRA" : "NV12", nullptr);
        gst_caps_set_features(result, i,
                              gst_caps_features_new(direction == GST_PAD_SINK
                                                        ? GST_CAPS_FEATURE_MEMORY_DMABUF
                                                        : GST_CAPS_FEATURE_MEMORY_VA,
                                                    nullptr));
        gst_structure_remove_field(gst_caps_get_structure(result, i), "colorimetry");
        gst_structure_remove_field(gst_caps_get_structure(result, i), "chroma-site");
    }
    if (filter) {
        auto *intersection = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(result);
        result = intersection;
    }
    return result;
}
static gboolean set_caps(GstBaseTransform *t, GstCaps *in, GstCaps *) {
    auto *s = reinterpret_cast<AmdWatermark *>(t);
    try {
        GstVideoInfo video{};
        if (!s->state || !gst_video_info_from_caps(&video, in) || video.width % 2 ||
            video.height % 2 || video.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
            (video.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN &&
             video.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709) ||
            (video.colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN &&
             video.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235))
            return FALSE;
        if (s->state->video.width &&
            (s->state->video.width != video.width || s->state->video.height != video.height)) {
            s->state->pool->flushing = true;
            s->state->pool->ready.notify_all();
            auto state = std::make_unique<State>();
            state->device = s->device;
            delete s->state;
            s->state = state.release();
        }
        s->state->video = video;
        return TRUE;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(s, LIBRARY, INIT, ("GPU watermark caps initialization failed"),
                          ("%s", e.what()));
        return FALSE;
    }
}
static GstFlowReturn prepare(GstBaseTransform *t, GstBuffer *input, GstBuffer **output) {
    auto *self = reinterpret_cast<AmdWatermark *>(t);
    try {
        auto &s = *self->state;
        amdva::hip_check(hipSetDevice(s.device), "Watermark device");
        auto *display = gst_va_buffer_peek_display(input);
        if (!display)
            throw std::runtime_error("Missing VA display");
        if (!s.display) {
            s.display = std::make_shared<Display>(display);
            s.va = std::make_shared<amdva::VaSession>(
                static_cast<VADisplay>(gst_va_display_get_va_dpy(display)));
            s.renderer = std::make_unique<amdva::OverlayRenderer>(s.device);
        }
        if (s.display->value != display)
            throw std::runtime_error("VA display changed");
        std::shared_ptr<Surface> slot;
        {
            std::unique_lock<std::mutex> lock(s.pool->mutex);
            for (;;) {
                if (s.pool->flushing)
                    return GST_FLOW_FLUSHING;
                for (auto &item : s.pool->slots)
                    if (!item->busy) {
                        slot = item;
                        break;
                    }
                if (slot)
                    break;
                if (s.pool->slots.size() < 4) {
                    slot = std::make_shared<Surface>();
                    slot->display = s.display;
                    slot->va = s.va;
                    slot->id = s.va->create_bgra(s.video.width, s.video.height);
                    auto descriptor = s.va->export_surface(slot->id, true);
                    slot->fd = dup(descriptor.objects[0].fd);
                    if (slot->fd < 0) {
                        for (unsigned i = 0; i < descriptor.num_objects; i++)
                            close(descriptor.objects[i].fd);
                        throw std::runtime_error("Cannot retain overlay DMA-BUF");
                    }
                    slot->image = std::make_unique<amdva::ImportedSurface>(descriptor);
                    s.pool->slots.push_back(slot);
                    break;
                }
                s.pool->ready.wait_for(lock, std::chrono::milliseconds(10));
            }
            slot->busy = true;
        }
        auto *buffer = gst_buffer_new();
        int fd = dup(slot->fd);
        if (fd < 0) {
            std::lock_guard<std::mutex> lock(s.pool->mutex);
            slot->busy = false;
            throw std::runtime_error("Cannot duplicate output DMA-BUF");
        }
        auto *memory =
            gst_dmabuf_allocator_alloc(s.allocator, fd, slot->image->layout().object_size);
        if (!memory) {
            close(fd);
            gst_buffer_unref(buffer);
            std::lock_guard<std::mutex> lock(s.pool->mutex);
            slot->busy = false;
            throw std::runtime_error("Cannot wrap overlay DMA-BUF");
        }
        gst_mini_object_weak_ref(GST_MINI_OBJECT(memory), release, new Release{s.pool, slot});
        gst_buffer_append_memory(buffer, memory);
        gst_buffer_copy_into(buffer, input, GST_BUFFER_COPY_METADATA, 0, -1);
        // Replace the NV12 layout meta copied from the input with the BGRA layout.
        if (auto *meta = gst_buffer_get_video_meta(buffer))
            gst_buffer_remove_meta(buffer, &meta->meta);
        gsize offsets[GST_VIDEO_MAX_PLANES] = {slot->image->layout().y_offset};
        gint strides[GST_VIDEO_MAX_PLANES] = {gint(slot->image->layout().y_pitch)};
        gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRA,
                                       s.video.width, s.video.height, 1, offsets, strides);
        // Stash the pool slot on the buffer for transform(), independent of metadata copying.
        gst_mini_object_set_qdata(GST_MINI_OBJECT(buffer),
                                  g_quark_from_static_string("amd-overlay-slot"), slot.get(),
                                  nullptr);
        *output = buffer;
        return GST_FLOW_OK;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED, ("GPU overlay allocation failed"),
                          ("%s", e.what()));
        return GST_FLOW_ERROR;
    }
}
static GstFlowReturn transform(GstBaseTransform *t, GstBuffer *input, GstBuffer *output) {
    auto *self = reinterpret_cast<AmdWatermark *>(t);
    try {
        auto &s = *self->state;
        auto *slot = static_cast<Surface *>(gst_mini_object_get_qdata(
            GST_MINI_OBJECT(output), g_quark_from_static_string("amd-overlay-slot")));
        if (!slot)
            throw std::runtime_error("Missing overlay pool slot");
        auto surface = gst_va_buffer_get_surface(input);
        if (surface == VA_INVALID_SURFACE)
            throw std::runtime_error("Missing decoded surface");
        auto before = amdva::engine_time_ns("vpe");
        s.va->scale(surface, slot->id, s.video.width, s.video.height, s.video.width,
                    s.video.height);
        if (amdva::engine_time_ns("vpe") <= before)
            throw std::runtime_error("VPE execution not observed");
        s.renderer->draw(*slot->image, s.video.width, s.video.height,
                         amdva::overlay_from_buffer(input, s.video.width, s.video.height));
        return GST_FLOW_OK;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED, ("GPU overlay failed"), ("%s", e.what()));
        return GST_FLOW_ERROR;
    }
}
static void finalize(GObject *o) {
    delete reinterpret_cast<AmdWatermark *>(o)->state;
    G_OBJECT_CLASS(amd_watermark_parent_class)->finalize(o);
}
static void amd_watermark_class_init(AmdWatermarkClass *k) {
    auto *object = G_OBJECT_CLASS(k);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                          GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(
        object, P_DEVICE, g_param_spec_int("device", "Device", "HIP device", 0, 16, 0, flags));
    g_object_class_install_property(
        object, P_STRICT,
        g_param_spec_boolean("require-zero-copy", "Strict", "GPU drawing only", TRUE, flags));
    auto *element = GST_ELEMENT_CLASS(k);
    gst_element_class_set_static_metadata(
        element, "AMD GPU watermark", "Filter/Video",
        "VPE BGRA conversion and HIP overlay into a bounded DMA-BUF pool", "AMD");
    gst_element_class_add_static_pad_template(element, &sink);
    gst_element_class_add_static_pad_template(element, &src);
    auto *base = GST_BASE_TRANSFORM_CLASS(k);
    base->start = start;
    base->stop = stop;
    base->sink_event = event;
    base->transform_caps = transform_caps;
    base->set_caps = set_caps;
    base->prepare_output_buffer = prepare;
    base->transform = transform;
}
static void amd_watermark_init(AmdWatermark *self) {
    self->strict = TRUE;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), FALSE);
}
