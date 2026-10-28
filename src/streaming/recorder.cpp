#include "amdva/streaming/recorder.hpp"
#include <algorithm>
#include <filesystem>
#include <gst/app/gstappsrc.h>
#include <stdexcept>

namespace amdva::streaming {
namespace {
std::string identifier() {
    gchar *p = g_uuid_string_random();
    std::string s = p;
    g_free(p);
    return s;
}
std::string message_error(GstMessage *m) {
    GError *e{};
    gchar *debug{};
    gst_message_parse_error(m, &e, &debug);
    std::string s = e ? e->message : "Pipeline error";
    g_clear_error(&e);
    g_free(debug);
    return s;
}
void destroy(GstElement *&p) {
    if (p) {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
        p = nullptr;
    }
}
const char *parser(RecordCodec codec) {
    switch (codec) {
    case RecordCodec::H264: return "h264parse ! video/x-h264,stream-format=avc,alignment=au";
    case RecordCodec::H265: return "h265parse ! video/x-h265,stream-format=hvc1,alignment=au";
    case RecordCodec::AV1: break;
    }
    return "av1parse ! video/x-av1,stream-format=obu-stream,alignment=tu";
}
} // namespace

SegmentRecorder::SegmentRecorder(Catalog &catalog, Options options, GstCaps *caps)
    : catalog_(catalog), options_(std::move(options)) {
    if (options_.session.empty())
        options_.session = identifier();
    GError *error{};
    pipeline_ = gst_parse_launch(
        (std::string("appsrc name=input is-live=true format=time block=false ! ") +
         parser(options_.codec) +
         " ! splitmuxsink name=segments muxer-factory=matroskamux async-finalize=true")
            .c_str(),
        &error);
    if (error || !pipeline_) {
        std::string text = error ? error->message : "Cannot create recorder pipeline";
        g_clear_error(&error);
        destroy(pipeline_);
        throw std::runtime_error(text);
    }
    input_ = gst_bin_get_by_name(GST_BIN(pipeline_), "input");
    gst_app_src_set_caps(GST_APP_SRC(input_), caps);
    g_object_set(input_, "max-bytes", guint64(options_.queue_limit_bytes), nullptr);
    auto *mux = gst_bin_get_by_name(GST_BIN(pipeline_), "segments");
    g_object_set(mux, "max-size-time", guint64(options_.segment_seconds) * GST_SECOND, nullptr);
    g_signal_connect(mux, "format-location-full", G_CALLBACK(location), this);
    gst_object_unref(mux);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_object_unref(input_);
        input_ = nullptr;
        destroy(pipeline_);
        throw std::runtime_error("Cannot start MKV recorder");
    }
}
SegmentRecorder::~SegmentRecorder() {
    if (options_.on_destroyed)
        options_.on_destroyed();
    if (input_)
        gst_object_unref(input_);
    destroy(pipeline_);
    for (const auto &f : files_) {
        try {
            catalog_.interrupted(f);
        } catch (...) {
        }
    }
}
gchar *SegmentRecorder::location(GstElement *, guint fragment, GstSample *first_sample,
                                 gpointer data) {
    auto &r = *static_cast<SegmentRecorder *>(data);
    const auto now = g_get_real_time();
    const auto name = std::to_string(now) + "-" + r.options_.channel + "-" + r.options_.session +
                      "-" + std::to_string(fragment) + ".mkv";
    try {
        std::string camera_name;
        {
            std::lock_guard<std::mutex> l(r.mutex_);
            camera_name = r.options_.camera_name;
        }
        std::optional<int64_t> media_start;
        if (first_sample && r.wall_origin_ms_ >= 0) {
            auto *buffer = gst_sample_get_buffer(first_sample);
            auto *segment = gst_sample_get_segment(first_sample);
            auto pts = buffer ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
            auto time = segment && GST_CLOCK_TIME_IS_VALID(pts)
                            ? gst_segment_to_stream_time(segment, GST_FORMAT_TIME, pts)
                            : pts;
            if (GST_CLOCK_TIME_IS_VALID(time))
                media_start = r.wall_origin_ms_ + time / GST_MSECOND;
        }
        r.catalog_.opened(name, r.options_.channel, camera_name, r.options_.session, fragment,
                          media_start);
        std::lock_guard<std::mutex> l(r.mutex_);
        r.files_.push_back(name);
    } catch (...) {
        r.failed_ = true;
        return g_strdup("/dev/full");
    }
    return g_strdup((r.catalog_.root() / name).c_str());
}
bool SegmentRecorder::push(GstSample *sample, std::optional<int64_t> wall_ms) {
    guint64 level{};
    g_object_get(input_, "current-level-bytes", &level, nullptr);
    if (level > options_.queue_limit_bytes)
        return false;
    auto *b = gst_buffer_copy(gst_sample_get_buffer(sample)); // Header only; memory stays shared.
    const auto pts = GST_BUFFER_PTS(b);
    if (!GST_CLOCK_TIME_IS_VALID(pts) || (origin_ != GST_CLOCK_TIME_NONE && pts < origin_)) {
        gst_buffer_unref(b);
        return false;
    }
    if (origin_ == GST_CLOCK_TIME_NONE) {
        origin_ = pts;
        wall_origin_ms_ = wall_ms ? *wall_ms : g_get_real_time() / 1000;
    }
    GST_BUFFER_PTS(b) = pts - origin_;
    GST_BUFFER_DTS(b) = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DTS(b)) && GST_BUFFER_DTS(b) >= origin_
                            ? GST_BUFFER_DTS(b) - origin_
                            : GST_CLOCK_TIME_NONE;
    return gst_app_src_push_buffer(GST_APP_SRC(input_), b) == GST_FLOW_OK;
}
void SegmentRecorder::end() {
    if (ending_ || !pipeline_)
        return;
    ending_ = true;
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    gst_app_src_end_of_stream(GST_APP_SRC(input_));
}
bool SegmentRecorder::poll() {
    if (!pipeline_)
        return true;
    auto *bus = gst_element_get_bus(pipeline_);
    bool done = false;
    while (auto *m = gst_bus_pop(bus)) {
        if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ELEMENT) {
            auto *s = gst_message_get_structure(m);
            if (s && gst_structure_has_name(s, "splitmuxsink-fragment-closed")) {
                const char *loc = gst_structure_get_string(s, "location");
                if (loc) {
                    const auto name = std::filesystem::path(loc).filename().string();
                    guint64 duration = GST_CLOCK_TIME_NONE;
                    gst_structure_get_uint64(s, "fragment-duration", &duration);
                    guint64 offset = GST_CLOCK_TIME_NONE;
                    gst_structure_get_uint64(s, "fragment-offset", &offset);
                    if (wall_origin_ms_ >= 0 && GST_CLOCK_TIME_IS_VALID(offset))
                        catalog_.media_start(name, wall_origin_ms_ + offset / GST_MSECOND);
                    catalog_.closed(name,
                                    GST_CLOCK_TIME_IS_VALID(duration) ? int64_t(duration) : -1);
                    std::lock_guard<std::mutex> lock(mutex_);
                    files_.erase(std::remove(files_.begin(), files_.end(), name), files_.end());
                }
            }
        } else if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = message_error(m);
            }
            failed_ = true;
            done = true;
        } else if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS)
            done = true;
        gst_message_unref(m);
    }
    gst_object_unref(bus);
    if (ending_ && std::chrono::steady_clock::now() >= deadline_)
        done = true;
    if (done) {
        destroy(pipeline_);
        std::lock_guard<std::mutex> l(mutex_);
        for (const auto &f : files_)
            catalog_.interrupted(f);
    }
    return done;
}
std::string SegmentRecorder::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}
void SegmentRecorder::camera_name(const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_.camera_name = name;
}
std::optional<int64_t> SegmentRecorder::wall_time_ms(GstClockTime pts) const {
    if (origin_ == GST_CLOCK_TIME_NONE || !GST_CLOCK_TIME_IS_VALID(pts) || pts < origin_)
        return std::nullopt;
    return wall_origin_ms_ + int64_t((pts - origin_) / GST_MSECOND);
}
std::vector<std::string> SegmentRecorder::open_segments() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return files_;
}
} // namespace amdva::streaming
