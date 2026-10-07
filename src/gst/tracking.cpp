#include "amdva/tracking.hpp"
#include <algorithm>
#include <gst/analytics/analytics.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvautils.h>
#include <gst/video/video.h>
namespace amdva {
struct MetadataTracker::Impl {
    ModelInfo info;
    std::string mode;
    GstElement *pipeline{}, *source{}, *sink{};
    GstCaps *caps{};
    uint64_t generation{};
    bool started{};
    Impl(ModelInfo m, std::string type) : info(std::move(m)), mode(std::move(type)) {
        if (mode != "short-term-imageless" && mode != "zero-term-imageless")
            throw std::runtime_error("Only imageless tracking is supported");
    }
    void reset() {
        if (pipeline)
            gst_element_set_state(pipeline, GST_STATE_NULL);
        if (source)
            gst_object_unref(source);
        if (sink)
            gst_object_unref(sink);
        if (pipeline)
            gst_object_unref(pipeline);
        if (caps)
            gst_caps_unref(caps);
        pipeline = source = sink = nullptr;
        caps = nullptr;
        started = false;
    }
    ~Impl() { reset(); }
    void initialize(GstCaps *input, uint64_t gen, GstBuffer *buffer) {
        reset();
        GError *error = nullptr;
        pipeline = gst_parse_launch(
            ("appsrc name=input format=time max-buffers=1 ! gvatrack tracking-type=" + mode +
             " ! appsink name=output sync=false max-buffers=1 async=false")
                .c_str(),
            &error);
        if (error) {
            std::string message = error->message;
            g_clear_error(&error);
            reset();
            throw std::runtime_error(message);
        }
        source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
        sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
        caps = gst_caps_ref(input);
        generation = gen;
        if (auto *display = gst_va_buffer_peek_display(buffer)) {
            auto *pad = gst_element_get_static_pad(source, "src");
            gst_pad_add_probe(
                pad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_QUERY_UPSTREAM |
                                             GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM),
                [](GstPad *, GstPadProbeInfo *info, gpointer data) {
                    auto *query = GST_PAD_PROBE_INFO_QUERY(info);
                    if (GST_QUERY_TYPE(query) != GST_QUERY_CONTEXT)
                        return GST_PAD_PROBE_OK;
                    const gchar *type = nullptr;
                    gst_query_parse_context_type(query, &type);
                    if (g_strcmp0(type, GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR))
                        return GST_PAD_PROBE_OK;
                    auto *context = gst_context_new(type, TRUE);
                    gst_context_set_va_display(context, GST_VA_DISPLAY(data));
                    gst_query_set_context(query, context);
                    gst_context_unref(context);
                    return GST_PAD_PROBE_HANDLED;
                },
                gst_object_ref(display), [](gpointer display) { gst_object_unref(display); });
            gst_object_unref(pad);
        }
        gst_app_src_set_caps(GST_APP_SRC(source), caps);
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Cannot start imageless tracker");
        started = true;
    }
};
MetadataTracker::MetadataTracker(ModelInfo info, const std::string &mode)
    : impl_(std::make_unique<Impl>(std::move(info), mode)) {}
MetadataTracker::~MetadataTracker() = default;
std::vector<TrackedObject> MetadataTracker::process(GstSample *sample,
                                                    const std::vector<Detection> *observations,
                                                    const FrameIdentity &identity) {
    auto &s = *impl_;
    auto *caps = gst_sample_get_caps(sample);
    auto *input = gst_sample_get_buffer(sample);
    GstVideoInfo video{};
    if (!caps || !input || !gst_video_info_from_caps(&video, caps))
        throw std::runtime_error("Invalid tracker input sample");
    if (!s.started || identity.generation != s.generation || !gst_caps_is_equal(s.caps, caps))
        s.initialize(caps, identity.generation, input);
    auto *buffer = gst_buffer_copy(input);
    if (!buffer)
        throw std::runtime_error("Cannot retain tracking frame");
    if (observations) {
        AnalysisResult result;
        for (const auto &box : *observations) {
            Instance object;
            object.box = box;
            result.objects.push_back(object);
        }
        attach_analysis(
            buffer, std::move(result), s.info,
            ImageTransform::make(video.width, video.height, video.width, video.height, false),
            identity);
    }
    if (gst_app_src_push_buffer(GST_APP_SRC(s.source), buffer) != GST_FLOW_OK)
        throw std::runtime_error("Tracking input failed");
    auto *output = gst_app_sink_try_pull_sample(GST_APP_SINK(s.sink), GST_SECOND);
    if (!output) {
        auto *bus = gst_element_get_bus(s.pipeline);
        auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (message) {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            std::string text = error ? error->message : "Tracking failed";
            if (debug)
                text += " : " + std::string(debug);
            g_clear_error(&error);
            g_free(debug);
            gst_message_unref(message);
            throw std::runtime_error(text);
        }
        throw std::runtime_error("Tracking output timed out");
    }
    std::unique_ptr<GstSample, decltype(&gst_sample_unref)> guard(output, gst_sample_unref);
    std::vector<TrackedObject> result;
    auto *meta = gst_buffer_get_analytics_relation_meta(gst_sample_get_buffer(output));
    gpointer cursor = nullptr;
    GstAnalyticsMtd m{};
    while (meta && gst_analytics_relation_meta_iterate(meta, &cursor,
                                                       gst_analytics_od_mtd_get_mtd_type(), &m)) {
        TrackedObject object;
        gint x, y, w, h;
        gfloat confidence;
        auto *od = reinterpret_cast<GstAnalyticsODMtd *>(&m);
        gst_analytics_od_mtd_get_location(od, &x, &y, &w, &h, &confidence);
        const auto *label = g_quark_to_string(gst_analytics_od_mtd_get_obj_type(od));
        object.label = label ? label : "";
        auto it = std::find(s.info.labels.begin(), s.info.labels.end(), object.label);
        object.box = {float(x),   float(y),
                      float(w),   float(h),
                      confidence, it == s.info.labels.end() ? -1 : int(it - s.info.labels.begin())};
        gpointer related = nullptr;
        GstAnalyticsMtd tracking{};
        if (gst_analytics_relation_meta_get_direct_related(
                meta, m.id, GST_ANALYTICS_REL_TYPE_ANY, gst_analytics_tracking_mtd_get_mtd_type(),
                &related, &tracking))
            gst_analytics_tracking_mtd_get_info(
                reinterpret_cast<GstAnalyticsTrackingMtd *>(&tracking), &object.id, nullptr,
                nullptr, nullptr);
        result.push_back(std::move(object));
    }
    return result;
}
} // namespace amdva
