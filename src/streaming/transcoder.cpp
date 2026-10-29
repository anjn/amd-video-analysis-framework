#include "amdva/streaming/transcoder.hpp"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/va/gstvautils.h>
#include <cmath>
#include <stdexcept>

namespace amdva::streaming {
namespace {
const char *encode_codec(RecordCodec codec) {
    switch (codec) {
    case RecordCodec::H264: return "h264";
    case RecordCodec::H265: return "h265";
    case RecordCodec::AV1: break;
    }
    return "av1";
}
const char *rate_control(RateControl mode) {
    switch (mode) {
    case RateControl::CBR: return "cbr";
    case RateControl::VBR: return "vbr";
    case RateControl::QVBR: break;
    }
    return "qvbr";
}
// Carries the source PTS through decode, scaling and encode. Encoders shift their
// output timeline (VA encoders by 1000 hours), so the output PTS is not reliable.
GstCaps *source_pts() {
    static GstCaps *caps = gst_caps_new_empty_simple("timestamp/x-amdva-source-pts");
    return caps;
}
} // namespace

Transcoder::Transcoder(GstVaDisplay *display, Options options, GstCaps *caps)
    : options_(std::move(options)) {
    const char *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    const bool h264 = g_str_equal(name, "video/x-h264");
    if (!h264 && !g_str_equal(name, "video/x-h265"))
        throw std::runtime_error(std::string("Transcoding needs H.264 or H.265 input, not ") +
                                 name);
    // Scaling keeps the aspect ratio and never enlarges. Both dimensions are given:
    // letting vapostproc derive the width crashes the radeonsi VA driver.
    int width = 0, height = 0;
    gst_structure_get_int(gst_caps_get_structure(caps, 0), "width", &width);
    gst_structure_get_int(gst_caps_get_structure(caps, 0), "height", &height);
    int out_width = width, out_height = height;
    if (options_.height && height && int(options_.height) < height) {
        out_height = int(options_.height);
        out_width = int(std::lround(double(out_height) * width / height / 2)) * 2;
    }
    // The AV1 encoder pads the width to 64 and mis-sizes heights that are not a
    // multiple of 16 (1080 becomes 1082), so AV1 output snaps to those multiples.
    if (options_.codec == RecordCodec::AV1 && out_width && out_height) {
        out_width = std::max(64, int(std::lround(out_width / 64.)) * 64);
        out_height = std::max(16, int(std::lround(out_height / 16.)) * 16);
    }
    if ((options_.height || options_.codec == RecordCodec::AV1) && (!width || !height))
        throw std::runtime_error("Scaling needs the input width and height");
    std::string scale;
    if (out_width != width || out_height != height)
        scale = " ! vapostproc ! video/x-raw(memory:VAMemory),format=NV12,width=" +
                std::to_string(out_width) + ",height=" + std::to_string(out_height);
    // The encoder queue holds a few frames so the decoder's surface pool is not
    // exhausted, and drops the oldest when the video engine falls behind: the
    // recording loses frames instead of decode stalling and the backlog growing until
    // the stream restarts. The frame branch keeps just the latest frame.
    const std::string queue =
        " ! queue name=pending max-size-buffers=4 max-size-bytes=0 max-size-time=0 "
        "leaky=downstream";
    const std::string decoded = options_.frames ? " ! tee name=split" + queue : queue;
    const std::string frames =
        options_.frames ? " split. ! appsink name=frames sync=false max-buffers=1 drop=true" : "";
    const std::string description =
        std::string("appsrc name=input is-live=true format=time block=false ! ") +
        (h264 ? "h264parse ! vah264dec name=decode" : "h265parse ! vah265dec name=decode") +
        " ! video/x-raw(memory:VAMemory),format=NV12" + decoded + scale + " ! amdencode codec=" +
        encode_codec(options_.codec) + " rate-control=" + rate_control(options_.rate_control) +
        " bitrate=" + std::to_string(options_.bitrate_kbps) +
        " key-int-max=" + std::to_string(options_.key_interval_frames) +
        " ! appsink name=output sync=false emit-signals=false max-buffers=0 drop=false" + frames;
    GError *error{};
    pipeline_ = gst_parse_launch(description.c_str(), &error);
    if (error || !pipeline_) {
        std::string text = error ? error->message : "Cannot create transcoder pipeline";
        g_clear_error(&error);
        if (pipeline_)
            gst_object_unref(pipeline_);
        throw std::runtime_error(text);
    }
    auto *context = gst_context_new(GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR, TRUE);
    gst_context_set_va_display(context, display);
    gst_element_set_context(pipeline_, context);
    gst_context_unref(context);
    input_ = gst_bin_get_by_name(GST_BIN(pipeline_), "input");
    output_ = gst_bin_get_by_name(GST_BIN(pipeline_), "output");
    decoder_ = gst_bin_get_by_name(GST_BIN(pipeline_), "decode");
    frames_ = gst_bin_get_by_name(GST_BIN(pipeline_), "frames");
    gst_app_src_set_caps(GST_APP_SRC(input_), caps);
    auto *pending = gst_bin_get_by_name(GST_BIN(pipeline_), "pending");
    auto *pad = gst_element_get_static_pad(pending, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, throttle, this, nullptr);
    gst_object_unref(pad);
    gst_object_unref(pending);
    g_object_set(input_, "max-bytes", guint64(options_.queue_limit_bytes), nullptr);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        poll();
        const auto text = error_.empty() ? std::string("Cannot start transcoder") : error_;
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        release();
        throw std::runtime_error(text);
    }
}
Transcoder::~Transcoder() {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    release();
}
void Transcoder::release() {
    for (auto *element : {input_, output_, decoder_, frames_, pipeline_})
        if (element)
            gst_object_unref(element);
}
bool Transcoder::push(GstSample *sample) {
    guint64 level{};
    g_object_get(input_, "current-level-bytes", &level, nullptr);
    if (level > options_.queue_limit_bytes)
        return false;
    auto *b = gst_buffer_copy(gst_sample_get_buffer(sample)); // Header only; memory shared.
    if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(b))) {
        gst_buffer_unref(b);
        return false;
    }
    gst_buffer_add_reference_timestamp_meta(b, source_pts(), GST_BUFFER_PTS(b),
                                            GST_CLOCK_TIME_NONE);
    if (options_.on_input)
        options_.on_input(b);
    pushed_[GST_BUFFER_PTS(b)] = g_get_monotonic_time();
    // Frames the decoder or encoder dropped would otherwise stay forever.
    while (pushed_.size() > 512)
        pushed_.erase(pushed_.begin());
    return gst_app_src_push_buffer(GST_APP_SRC(input_), b) == GST_FLOW_OK;
}
void Transcoder::max_fps(double fps) {
    min_interval_ = fps > 0 ? GstClockTime(GST_SECOND / fps) : 0;
}
GstPadProbeReturn Transcoder::throttle(GstPad *, GstPadProbeInfo *info, gpointer data) {
    auto &t = *static_cast<Transcoder *>(data);
    const auto interval = t.min_interval_.load();
    const auto pts = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info));
    if (!interval || !GST_CLOCK_TIME_IS_VALID(pts)) {
        t.last_kept_ = pts;
        return GST_PAD_PROBE_OK;
    }
    // A small tolerance keeps exact divisors of the camera rate (15 of 30 fps).
    if (GST_CLOCK_TIME_IS_VALID(t.last_kept_) && pts > t.last_kept_ &&
        pts - t.last_kept_ < interval - interval / 8)
        return GST_PAD_PROBE_DROP;
    t.last_kept_ = pts;
    return GST_PAD_PROBE_OK;
}
GstSample *Transcoder::pull() {
    auto *encoded = gst_app_sink_try_pull_sample(GST_APP_SINK(output_), 0);
    if (!encoded)
        return nullptr;
    auto *buffer = gst_buffer_copy(gst_sample_get_buffer(encoded)); // Header only.
    auto *meta = gst_buffer_get_reference_timestamp_meta(buffer, source_pts());
    // No B-frames are produced, so decode order equals presentation order.
    GST_BUFFER_PTS(buffer) = GST_BUFFER_DTS(buffer) =
        meta ? meta->timestamp : GST_CLOCK_TIME_NONE;
    auto *sample = gst_sample_new(buffer, gst_sample_get_caps(encoded), nullptr, nullptr);
    gst_buffer_unref(buffer);
    gst_sample_unref(encoded);
    encoded_++;
    const auto pts = GST_BUFFER_PTS(gst_sample_get_buffer(sample));
    auto it = pushed_.find(pts);
    if (it != pushed_.end()) {
        const double ms = (g_get_monotonic_time() - it->second) / 1000.;
        latency_ms_ = latency_ms_ ? latency_ms_ * .9 + ms * .1 : ms;
        pushed_.erase(pushed_.begin(), std::next(it));
    }
    return sample;
}
bool Transcoder::poll() {
    auto *bus = gst_element_get_bus(pipeline_);
    bool ok = true;
    while (auto *m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
        GError *error{};
        gst_message_parse_error(m, &error, nullptr);
        error_ = error ? error->message : "Transcoder error";
        g_clear_error(&error);
        gst_message_unref(m);
        ok = false;
    }
    gst_object_unref(bus);
    return ok;
}
} // namespace amdva::streaming
