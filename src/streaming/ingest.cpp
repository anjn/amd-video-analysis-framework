#include "amdva/streaming/ingest.hpp"
#include <gst/app/gstappsink.h>
#include <stdexcept>

namespace amdva::streaming {
namespace {
std::string message_error(GstMessage *m) {
    GError *e{};
    gchar *debug{};
    gst_message_parse_error(m, &e, &debug);
    std::string s = e ? e->message : "Pipeline error";
    g_clear_error(&e);
    g_free(debug);
    return s;
}
Sample own(GstSample *s) {
    return Sample(s, [](GstSample *p) { gst_sample_unref(p); });
}
} // namespace

RtspIngest::RtspIngest(Options options, Hooks hooks)
    : options_(std::move(options)), hooks_(std::move(hooks)) {}
RtspIngest::~RtspIngest() { stop(); }

GstFlowReturn RtspIngest::received(GstAppSink *sink, gpointer data) {
    auto &c = *static_cast<RtspIngest *>(data);
    auto sample = own(gst_app_sink_pull_sample(sink));
    if (!sample)
        return GST_FLOW_EOS;
    auto *b = gst_sample_get_buffer(sample.get());
    const auto size = gst_buffer_get_size(b);
    // Count before queue drops and before any consumer selection.
    c.frames_.fetch_add(1, std::memory_order_relaxed);
    c.bytes_.fetch_add(size, std::memory_order_relaxed);
    c.last_packet_us_ = g_get_monotonic_time();
    const auto limit = c.options_.queue_limit_bytes;
    std::lock_guard<std::mutex> l(c.mutex_);
    if (c.queued_ + size > limit ||
        (!c.queue_.empty() && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(b)) &&
         GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(gst_sample_get_buffer(c.queue_.front().get()))) &&
         GST_BUFFER_PTS(b) >
             GST_BUFFER_PTS(gst_sample_get_buffer(c.queue_.front().get())) + 2 * GST_SECOND)) {
        if (c.hooks_.on_discarded)
            c.hooks_.on_discarded(c.queue_.size());
        c.queue_.clear();
        c.queued_ = 0;
        c.overflow_ = true;
    }
    // An oversized AU must not bypass the queue's hard cap.
    if (size > limit) {
        if (c.hooks_.on_discarded)
            c.hooks_.on_discarded(1);
    } else {
        c.queued_ += size;
        c.queue_.push_back(std::move(sample));
    }
    return GST_FLOW_OK;
}

void RtspIngest::pad_added(GstElement *, GstPad *pad, gpointer data) {
    auto &c = *static_cast<RtspIngest *>(data);
    auto *caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, nullptr);
    if (!caps || gst_caps_is_empty(caps)) {
        if (caps)
            gst_caps_unref(caps);
        return;
    }
    auto *s = gst_caps_get_structure(caps, 0);
    const char *media = gst_structure_get_string(s, "media"),
               *enc = gst_structure_get_string(s, "encoding-name");
    if (!media || std::string(media) != "video") {
        gst_caps_unref(caps);
        return;
    }
    const int codec = enc && std::string(enc) == "H264"   ? 1
                      : enc && std::string(enc) == "H265" ? 2
                                                          : -1;
    gst_caps_unref(caps);
    int expected = 0;
    if (!c.codec_.compare_exchange_strong(expected, codec) || codec < 0)
        return;
    GError *e{};
    const char *desc = codec == 1 ? "rtph264depay ! h264parse config-interval=-1 ! "
                                    "video/x-h264,stream-format=byte-stream,alignment=au ! "
                                    "appsink name=encoded sync=false max-buffers=1"
                                  : "rtph265depay ! h265parse config-interval=-1 ! "
                                    "video/x-h265,stream-format=byte-stream,alignment=au ! "
                                    "appsink name=encoded sync=false max-buffers=1";
    auto *branch = gst_parse_bin_from_description(desc, TRUE, &e);
    if (e || !branch) {
        g_clear_error(&e);
        if (branch)
            gst_object_unref(branch);
        c.codec_ = -1;
        return;
    }
    auto *sink = gst_bin_get_by_name(GST_BIN(branch), "encoded");
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = received;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, &c, nullptr);
    if (c.hooks_.on_access_unit_pad) {
        auto *encoded_pad = gst_element_get_static_pad(sink, "sink");
        c.hooks_.on_access_unit_pad(encoded_pad);
        gst_object_unref(encoded_pad);
    }
    gst_object_unref(sink);
    gst_bin_add(GST_BIN(c.pipeline_), branch);
    auto *input = gst_element_get_static_pad(branch, "sink");
    if (gst_pad_link(pad, input) != GST_PAD_LINK_OK)
        c.codec_ = -1;
    gst_object_unref(input);
    gst_element_sync_state_with_parent(branch);
}

void RtspIngest::start() {
    stop();
    codec_ = 0;
    overflow_ = false;
    frames_ = 0;
    bytes_ = 0;
    pipeline_ = gst_pipeline_new(nullptr);
    auto *src = gst_element_factory_make("rtspsrc", nullptr);
    if (!src) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        throw std::runtime_error("rtspsrc unavailable");
    }
    g_object_set(src, "location", options_.uri.c_str(), "protocols", 4, "latency",
                 options_.latency_ms, "tcp-timeout", guint64(5 * G_USEC_PER_SEC),
                 "do-rtsp-keep-alive", TRUE, nullptr);
    if (hooks_.on_source)
        hooks_.on_source(src);
    g_signal_connect(src, "pad-added", G_CALLBACK(pad_added), this);
    gst_bin_add(GST_BIN(pipeline_), src);
    last_packet_us_ = g_get_monotonic_time();
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        stop();
        throw std::runtime_error("Cannot start RTSP receiver");
    }
}
void RtspIngest::stop() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    frames_ = 0;
    bytes_ = 0;
    std::lock_guard<std::mutex> l(mutex_);
    queue_.clear();
    queued_ = 0;
}
std::string RtspIngest::poll() {
    if (!pipeline_)
        return {};
    auto *bus = gst_element_get_bus(pipeline_);
    std::string error;
    while (auto *m = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
        error = GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR ? message_error(m) : "RTSP ended";
        gst_message_unref(m);
    }
    gst_object_unref(bus);
    if (codec_ == -1)
        error = "Unsupported video codec";
    if (g_get_monotonic_time() - last_packet_us_.load() >
        gint64(options_.timeout_seconds) * G_USEC_PER_SEC)
        error = "RTSP video timeout";
    return error;
}
std::deque<Sample> RtspIngest::take() {
    std::deque<Sample> out;
    std::lock_guard<std::mutex> l(mutex_);
    out.swap(queue_);
    queued_ = 0;
    return out;
}
} // namespace amdva::streaming
