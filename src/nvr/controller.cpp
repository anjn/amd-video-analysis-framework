#include "controller.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/va/gstva.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay_drm.h>
#include <gst/va/gstvautils.h>
#include <iostream>
#include <malloc.h>
#include <map>
#include <random>
namespace amdva::nvr {
void initialize_runtime() {
    // Reuse a bounded set of glibc arenas across transient camera worker groups.
    // Respect an operator's explicit allocator setting.
    const auto *arenas = std::getenv("MALLOC_ARENA_MAX");
    if (!arenas || !*arenas)
        mallopt(M_ARENA_MAX, 4);
    gst_init(nullptr, nullptr);
}
namespace {
constexpr uint64_t compressed_limit = 16ull << 20;
GstElement *parse(const std::string &description) {
    GError *error{};
    auto *p = gst_parse_launch(description.c_str(), &error);
    if (error) {
        std::string text = error->message;
        g_clear_error(&error);
        if (p)
            gst_object_unref(p);
        throw std::runtime_error(text);
    }
    if (!p)
        throw std::runtime_error("Cannot create pipeline");
    return p;
}
void destroy(GstElement *&p) {
    if (p) {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
        p = nullptr;
    }
}
Sample own(GstSample *s) {
    return Sample(s, [](GstSample *p) { gst_sample_unref(p); });
}
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
// Never return raw plugin messages to the UI: rtspsrc errors may include credentials.
std::string public_error(const std::string &error, const Source &source) {
    if (error.find(source.uri) != std::string::npos || error.find("rtsp://") != std::string::npos ||
        error.find("rtsps://") != std::string::npos)
        return "RTSP operation failed";
    return error;
}
} // namespace
Slot::~Slot() {
    preview_va.reset();
    image.reset();
    if (va) {
        for (auto &p : preview)
            va->destroy_surface(p.surface);
        va->destroy_surface(ai_surface);
    }
}
struct Controller::Channel {
    struct Recorder {
        Channel *owner{};
        GstElement *pipe{};
        GstElement *input{};
        std::string session{identifier()};
        GstClockTime origin{GST_CLOCK_TIME_NONE};
        bool ending{};
        Clock::time_point deadline;
        std::mutex mutex;
        std::vector<std::string> files;
        ~Recorder() {
            owner->controller.request_reclaim();
            if (input)
                gst_object_unref(input);
            destroy(pipe);
            for (const auto &f : files) {
                try {
                    owner->controller.catalog_->closed(f);
                } catch (...) {
                }
            }
        }
        static gchar *location(GstElement *, guint fragment, gpointer data) {
            auto &r = *static_cast<Recorder *>(data);
            const auto now = g_get_real_time();
            const auto name = std::to_string(now) + "-" + r.owner->state.source.id + "-" +
                              r.session + "-" + std::to_string(fragment) + ".mkv";
            try {
                r.owner->controller.catalog_->opened(name, r.owner->state.source.id);
                std::lock_guard<std::mutex> l(r.mutex);
                r.files.push_back(name);
            } catch (...) {
                r.owner->record_failed = true;
                return g_strdup("/dev/full");
            }
            return g_strdup((r.owner->controller.catalog_->root() / name).c_str());
        }
        void end() {
            if (ending)
                return;
            ending = true;
            deadline = Clock::now() + std::chrono::seconds(5);
            gst_app_src_end_of_stream(GST_APP_SRC(input));
        }
        bool poll() {
            auto *bus = gst_element_get_bus(pipe);
            bool done = false;
            while (auto *m = gst_bus_pop(bus)) {
                if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ELEMENT) {
                    auto *s = gst_message_get_structure(m);
                    if (s && gst_structure_has_name(s, "splitmuxsink-fragment-closed")) {
                        const char *loc = gst_structure_get_string(s, "location");
                        if (loc) {
                            const auto name = std::filesystem::path(loc).filename().string();
                            owner->controller.catalog_->closed(name);
                            std::lock_guard<std::mutex> lock(mutex);
                            files.erase(std::remove(files.begin(), files.end(), name), files.end());
                        }
                    }
                } else if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
                    owner->record_failed = true;
                    owner->state.recording = "error";
                    owner->state.error =
                        "Recording: " + public_error(message_error(m), owner->state.source);
                    done = true;
                } else if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS)
                    done = true;
                gst_message_unref(m);
            }
            gst_object_unref(bus);
            if (ending && Clock::now() >= deadline)
                done = true;
            if (done) {
                destroy(pipe);
                std::lock_guard<std::mutex> l(mutex);
                for (const auto &f : files)
                    owner->controller.catalog_->closed(f);
            }
            return done;
        }
    };
    Controller &controller;
    Status state;
    GstElement *receive{}, *decoder{}, *decode_input{};
    std::vector<std::unique_ptr<Recorder>> recordings;
    Recorder *recording{};
    std::mutex encoded_mutex;
    std::deque<Sample> encoded;
    uint64_t queued{};
    std::atomic<int> codec{0};
    std::atomic<bool> overflow{false}, record_failed{false};
    std::atomic<int64_t> last_packet_us{0};
    bool removed{}, decode_wait{true};
    GstClockTime decode_origin{GST_CLOCK_TIME_NONE};
    unsigned retries{};
    Clock::time_point reconnect{Clock::now()}, record_retry{};
    uint64_t decode_count_base{}, ai_count_base{};
    Channel(Controller &c, Source source) : controller(c) {
        state.source = std::move(source);
        state.source.selected = false;
    }
    ~Channel() {
        stop_receive();
        stop_decode();
        recordings.clear();
    }
    static GstFlowReturn received(GstAppSink *sink, gpointer data) {
        auto &c = *static_cast<Channel *>(data);
        auto sample = own(gst_app_sink_pull_sample(sink));
        if (!sample)
            return GST_FLOW_EOS;
        auto *b = gst_sample_get_buffer(sample.get());
        const auto size = gst_buffer_get_size(b);
        c.last_packet_us = g_get_monotonic_time();
        std::lock_guard<std::mutex> l(c.encoded_mutex);
        if (c.queued + size > compressed_limit ||
            (!c.encoded.empty() && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(b)) &&
             GST_CLOCK_TIME_IS_VALID(
                 GST_BUFFER_PTS(gst_sample_get_buffer(c.encoded.front().get()))) &&
             GST_BUFFER_PTS(b) >
                 GST_BUFFER_PTS(gst_sample_get_buffer(c.encoded.front().get())) + 2 * GST_SECOND)) {
            c.encoded.clear();
            c.queued = 0;
            c.overflow = true;
        }
        // An oversized AU must not bypass the compressed queue's hard cap.
        if (size <= compressed_limit) {
            c.queued += size;
            c.encoded.push_back(std::move(sample));
        }
        return GST_FLOW_OK;
    }
    static void pad_added(GstElement *, GstPad *pad, gpointer data) {
        auto &c = *static_cast<Channel *>(data);
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
        if (!c.codec.compare_exchange_strong(expected, codec) || codec < 0)
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
            c.codec = -1;
            return;
        }
        auto *sink = gst_bin_get_by_name(GST_BIN(branch), "encoded");
        GstAppSinkCallbacks callbacks{};
        callbacks.new_sample = received;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, &c, nullptr);
        gst_object_unref(sink);
        gst_bin_add(GST_BIN(c.receive), branch);
        auto *input = gst_element_get_static_pad(branch, "sink");
        if (gst_pad_link(pad, input) != GST_PAD_LINK_OK)
            c.codec = -1;
        gst_object_unref(input);
        gst_element_sync_state_with_parent(branch);
    }
    static GstFlowReturn decoded(GstAppSink *sink, gpointer data) {
        auto &c = *static_cast<Channel *>(data);
        auto s = own(gst_app_sink_pull_sample(sink));
        if (!s)
            return GST_FLOW_EOS;
        GstVideoInfo info{};
        if (!gst_video_info_from_caps(&info, gst_sample_get_caps(s.get())) ||
            GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 || info.width > 1920 ||
            info.height > 1080 || info.width % 2 || info.height % 2 ||
            info.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
            (info.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN &&
             info.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709) ||
            (info.colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN &&
             info.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235))
            return GST_FLOW_NOT_NEGOTIATED;
        // slot and generation cannot change until set_state(NULL) has joined this callback.
        const int index = c.state.slot;
        if (index < 0)
            return GST_FLOW_OK;
        auto *crop = gst_buffer_get_video_crop_meta(gst_sample_get_buffer(s.get()));
        if (crop && (crop->x || crop->y || crop->width != unsigned(info.width) ||
                     crop->height != unsigned(info.height)))
            return GST_FLOW_NOT_NEGOTIATED;
        auto &slot = c.controller.slots_[index];
        std::lock_guard<std::mutex> l(slot.mutex);
        if (slot.latest.sample && (slot.latest.width != unsigned(info.width) ||
                                   slot.latest.height != unsigned(info.height)))
            slot.detections.clear();
        slot.latest = {std::move(s), c.state.generation, unsigned(info.width),
                       unsigned(info.height), Clock::now()};
        return GST_FLOW_OK;
    }
    void start_receive() {
        try {
            codec = 0;
            state.connection = "connecting";
            state.error.clear();
            state.generation = ++controller.next_generation_;
            if (state.slot >= 0) {
                auto &slot = controller.slots_[state.slot];
                std::lock_guard<std::mutex> l(slot.mutex);
                slot.generation = state.generation;
                slot.latest = {};
                slot.detections.clear();
            }
            receive = gst_pipeline_new(nullptr);
            auto *src = gst_element_factory_make("rtspsrc", nullptr);
            if (!src)
                throw std::runtime_error("rtspsrc unavailable");
            g_object_set(src, "location", state.source.uri.c_str(), "protocols", 4, "latency", 100,
                         "tcp-timeout", guint64(5 * G_USEC_PER_SEC), "do-rtsp-keep-alive", TRUE,
                         nullptr);
            g_signal_connect(src, "pad-added", G_CALLBACK(pad_added), this);
            gst_bin_add(GST_BIN(receive), src);
            last_packet_us = g_get_monotonic_time();
            if (gst_element_set_state(receive, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
                throw std::runtime_error("Cannot start RTSP receiver");
        } catch (const std::exception &e) {
            failed(e.what());
        }
    }
    void stop_receive() {
        if (receive)
            controller.request_reclaim();
        destroy(receive);
        std::lock_guard<std::mutex> l(encoded_mutex);
        encoded.clear();
        queued = 0;
    }
    void stop_decode() {
        if (decoder)
            controller.request_reclaim();
        if (decode_input) {
            gst_object_unref(decode_input);
            decode_input = nullptr;
        }
        destroy(decoder);
        decode_wait = true;
        decode_origin = GST_CLOCK_TIME_NONE;
    }
    void end_record() {
        if (recording) {
            recording->end();
            recording = nullptr;
        }
    }
    void failed(const std::string &error) {
        if (state.received && recording)
            state.gaps++;
        state.error = public_error(error, state.source);
        stop_receive();
        stop_decode();
        end_record();
        state.connection = "reconnecting";
        state.recording = "waiting";
        state.generation = ++controller.next_generation_;
        if (state.slot >= 0) {
            auto &s = controller.slots_[state.slot];
            std::lock_guard<std::mutex> l(s.mutex);
            s.generation = state.generation;
            s.latest = {};
            s.detections.clear();
        }
        const unsigned backoff = std::min(30u, 1u << std::min(retries++, 5u));
        const double jitter = 0.9 + (g_random_int() % 201) / 1000.0;
        reconnect = Clock::now() + std::chrono::milliseconds(unsigned(backoff * 1000 * jitter));
    }
    static GstPadProbeReturn allocation(GstPad *, GstPadProbeInfo *info, gpointer) {
        auto *query = GST_PAD_PROBE_INFO_QUERY(info);
        if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION)
            return GST_PAD_PROBE_OK;
        gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
        gst_query_add_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, nullptr);
        return GST_PAD_PROBE_HANDLED;
    }
    void start_decoder(GstCaps *caps) {
        const char *decode =
            codec == 1 ? "appsrc name=input is-live=true format=time block=false ! h264parse ! "
                         "vah264dec ! video/x-raw(memory:VAMemory),format=NV12 ! appsink "
                         "name=frames sync=false max-buffers=1 drop=true"
                       : "appsrc name=input is-live=true format=time block=false ! h265parse ! "
                         "vah265dec ! video/x-raw(memory:VAMemory),format=NV12 ! appsink "
                         "name=frames sync=false max-buffers=1 drop=true";
        decoder = parse(decode);
        auto *context = gst_context_new(GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR, TRUE);
        gst_context_set_va_display(context, controller.display_);
        gst_element_set_context(decoder, context);
        gst_context_unref(context);
        decode_input = gst_bin_get_by_name(GST_BIN(decoder), "input");
        gst_app_src_set_caps(GST_APP_SRC(decode_input), caps);
        g_object_set(decode_input, "max-bytes", guint64(compressed_limit), nullptr);
        auto *sink = gst_bin_get_by_name(GST_BIN(decoder), "frames");
        auto *pad = gst_element_get_static_pad(sink, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, allocation, nullptr, nullptr);
        gst_object_unref(pad);
        GstAppSinkCallbacks cb{};
        cb.new_sample = decoded;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, this, nullptr);
        gst_object_unref(sink);
        if (gst_element_set_state(decoder, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Cannot start VA decoder");
        decode_wait = false;
    }
    void start_record(GstCaps *caps) {
        auto r = std::make_unique<Recorder>();
        r->owner = this;
        r->pipe =
            parse(std::string("appsrc name=input is-live=true format=time block=false ! ") +
                  (codec == 1 ? "h264parse ! video/x-h264,stream-format=avc,alignment=au"
                              : "h265parse ! video/x-h265,stream-format=hvc1,alignment=au") +
                  " ! splitmuxsink name=segments muxer-factory=matroskamux async-finalize=true");
        r->input = gst_bin_get_by_name(GST_BIN(r->pipe), "input");
        gst_app_src_set_caps(GST_APP_SRC(r->input), caps);
        g_object_set(r->input, "max-bytes", guint64(compressed_limit), nullptr);
        auto *mux = gst_bin_get_by_name(GST_BIN(r->pipe), "segments");
        g_object_set(mux, "max-size-time",
                     guint64(controller.options_.segment_seconds) * GST_SECOND, nullptr);
        g_signal_connect(mux, "format-location", G_CALLBACK(Recorder::location), r.get());
        gst_object_unref(mux);
        if (gst_element_set_state(r->pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Cannot start MKV recorder");
        recording = r.get();
        recordings.push_back(std::move(r));
        state.recording = "recording";
        if (state.error.rfind("Recording:", 0) == 0 || state.error.rfind("Storage:", 0) == 0)
            state.error.clear();
    }
    bool push(GstElement *input, Sample &sample, GstClockTime &origin) {
        guint64 level{};
        g_object_get(input, "current-level-bytes", &level, nullptr);
        if (level > compressed_limit)
            return false;
        auto *original = gst_sample_get_buffer(sample.get());
        auto *b = gst_buffer_copy(original); // Header only; GstMemory refs stay shared.
        const auto pts = GST_BUFFER_PTS(b);
        if (!GST_CLOCK_TIME_IS_VALID(pts)) {
            gst_buffer_unref(b);
            return false;
        }
        if (origin == GST_CLOCK_TIME_NONE)
            origin = pts;
        if (pts < origin) {
            gst_buffer_unref(b);
            return false;
        }
        GST_BUFFER_PTS(b) = pts - origin;
        GST_BUFFER_DTS(b) =
            GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DTS(b)) && GST_BUFFER_DTS(b) >= origin
                ? GST_BUFFER_DTS(b) - origin
                : GST_CLOCK_TIME_NONE;
        return gst_app_src_push_buffer(GST_APP_SRC(input), b) == GST_FLOW_OK;
    }
    void tick(bool space) {
        recordings.erase(std::remove_if(recordings.begin(), recordings.end(),
                                        [this](auto &r) {
                                            if (r->poll()) {
                                                if (recording == r.get())
                                                    recording = nullptr;
                                                return true;
                                            }
                                            return false;
                                        }),
                         recordings.end());
        if (record_failed.exchange(false)) {
            end_record();
            state.recording = "error";
            state.gaps++;
            record_retry = Clock::now() + std::chrono::seconds(5);
        }
        if (removed)
            return;
        if (!space) {
            end_record();
            state.recording = "quota paused";
        }
        if (!receive) {
            if (Clock::now() >= reconnect)
                start_receive();
            return;
        }
        auto *bus = gst_element_get_bus(receive);
        std::string error;
        while (auto *m =
                   gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
            error = GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR ? message_error(m) : "RTSP ended";
            gst_message_unref(m);
        }
        gst_object_unref(bus);
        if (codec == -1)
            error = "Unsupported video codec";
        if (g_get_monotonic_time() - last_packet_us.load() > 5 * G_USEC_PER_SEC)
            error = "RTSP video timeout";
        if (!error.empty()) {
            failed(error);
            return;
        }
        if (decoder) {
            auto *b = gst_element_get_bus(decoder);
            while (auto *m = gst_bus_pop_filtered(
                       b, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
                state.error = "Decoder stopped; waiting for keyframe";
                gst_message_unref(m);
                stop_decode();
            }
            gst_object_unref(b);
        }
        if (overflow.exchange(false)) {
            stop_decode();
            end_record();
            state.gaps++;
        }
        std::deque<Sample> packets;
        {
            std::lock_guard<std::mutex> l(encoded_mutex);
            packets.swap(encoded);
            queued = 0;
        }
        for (auto &sample : packets) {
            state.received++;
            if (state.connection != "live")
                controller.request_reclaim();
            state.connection = "live";
            retries = 0;
            const bool key = !GST_BUFFER_FLAG_IS_SET(gst_sample_get_buffer(sample.get()),
                                                     GST_BUFFER_FLAG_DELTA_UNIT);
            try {
                if (space && Clock::now() >= record_retry) {
                    if (!recording && key)
                        start_record(gst_sample_get_caps(sample.get()));
                    if (recording && !push(recording->input, sample, recording->origin)) {
                        end_record();
                        state.gaps++;
                        state.recording = "waiting for keyframe";
                    }
                }
            } catch (const std::exception &e) {
                state.recording = "error";
                state.error = public_error(e.what(), state.source);
                end_record();
                state.gaps++;
                record_retry = Clock::now() + std::chrono::seconds(5);
            }
            if (state.slot >= 0) {
                try {
                    if (!decoder && key)
                        start_decoder(gst_sample_get_caps(sample.get()));
                    if (decoder && !push(decode_input, sample, decode_origin)) {
                        stop_decode();
                        state.error = "Decoder backlog; waiting for keyframe";
                    }
                } catch (const std::exception &e) {
                    stop_decode();
                    state.error = e.what();
                }
            }
        }
        if (state.slot >= 0) {
            auto &slot = controller.slots_[state.slot];
            std::lock_guard<std::mutex> l(slot.mutex);
            state.decoded = slot.processed - decode_count_base;
            state.ai = slot.inferred - ai_count_base;
            state.ai_age_ms = slot.detected_at == Clock::time_point{}
                                  ? 0
                                  : unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 Clock::now() - slot.detected_at)
                                                 .count());
            if (!slot.error.empty())
                state.error = slot.error;
        }
    }
};
Controller::Controller(Options options) : options_(std::move(options)) {
    if (options_.batch != 1 && options_.batch != 2 && options_.batch != 4 && options_.batch != 8 &&
        options_.batch != 16)
        throw std::runtime_error("Invalid batch size");
    if (!options_.segment_seconds || !options_.quota)
        throw std::runtime_error("Invalid recording limits");
    display_ = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    if (!display_)
        throw std::runtime_error("Cannot open shared VA display");
    try {
        catalog_ = std::make_unique<Catalog>(options_.recording_root);
        for (auto &s : slots_) {
            s.va = std::make_unique<VaSession>(
                static_cast<VADisplay>(gst_va_display_get_va_dpy(display_)));
            s.ai_surface = s.va->create_bgra(640, 640);
            s.image = std::make_unique<ImportedSurface>(s.va->export_surface(s.ai_surface));
            for (auto &p : s.preview)
                p.surface = s.va->create_bgra(1920, 1080);
        }
        // Keep the startup probe separate from the live processing contexts.
        VaSession gate(static_cast<VADisplay>(gst_va_display_get_va_dpy(display_)));
        const auto test = gate.create_nv12(640, 360, true), output = gate.create_bgra(640, 360);
        try {
            const auto before = engine_time_ns("vpe");
            gate.scale(test, output, 640, 360, 640, 360);
            if (engine_time_ns("vpe") <= before)
                throw std::runtime_error("Dedicated VPE scaling could not be observed");
        } catch (...) {
            gate.destroy_surface(test);
            gate.destroy_surface(output);
            throw;
        }
        gate.destroy_surface(test);
        gate.destroy_surface(output);
    } catch (...) {
        for (auto &s : slots_) {
            s.image.reset();
            if (s.va) {
                for (auto &p : s.preview) {
                    s.va->destroy_surface(p.surface);
                    p.surface = VA_INVALID_SURFACE;
                }
                s.va->destroy_surface(s.ai_surface);
                s.ai_surface = VA_INVALID_SURFACE;
                s.va.reset();
            }
        }
        gst_object_unref(display_);
        display_ = nullptr;
        throw;
    }
    control_ = std::thread([this] { loop(); });
    gpu_ = std::thread([this] { gpu_loop(); });
    preview_ = std::thread([this] { preview_loop(); });
    for (const auto &s : options_.sources)
        add(s);
    initial_enqueued_ = true;
}
Controller::~Controller() {
    shutdown();
    for (auto &s : slots_) {
        s.latest = {};
        s.preview_va.reset();
        s.image.reset();
        for (auto &p : s.preview) {
            s.va->destroy_surface(p.surface);
            p.surface = VA_INVALID_SURFACE;
        }
        s.va->destroy_surface(s.ai_surface);
        s.ai_surface = VA_INVALID_SURFACE;
        s.va.reset();
    }
    if (display_)
        gst_object_unref(display_);
}
void Controller::shutdown() {
    if (stopping_.exchange(true))
        return;
    if (control_.joinable())
        control_.join();
    if (gpu_.joinable())
        gpu_.join();
    if (preview_.joinable())
        preview_.join();
    channels_.clear();
}
void Controller::request_reclaim() {
    // Transient GStreamer worker groups leave free pages in glibc arenas. Allow
    // queued frame owners to drain, then return those pages without changing
    // the resource guard or retaining every reconnect's high-water mark.
    reclaim_pending_ = true;
    reclaim_at_ = std::max(reclaim_at_, Clock::now() + std::chrono::seconds(1));
}
void Controller::command(std::function<void()> f) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!stopping_)
        commands_.push_back(std::move(f));
}
void Controller::add(Source source) {
    command([this, source]() mutable {
        if (source.id.empty())
            source.id = identifier();
        if (source.id.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos)
            return;
        if (source.uri.rfind("rtsp://", 0) != 0 && source.uri.rfind("rtsps://", 0) != 0)
            return;
        for (const auto &c : channels_)
            if (c->state.source.id == source.id && !c->removed)
                return;
        const bool chosen = source.selected;
        auto c = std::make_unique<Channel>(*this, std::move(source));
        if (chosen)
            assign(*c, true);
        channels_.push_back(std::move(c));
    });
}
void Controller::remove(const std::string &id) {
    command([this, id] {
        for (auto &c : channels_)
            if (c->state.source.id == id && !c->removed) {
                assign(*c, false);
                c->removed = true;
                c->stop_receive();
                c->end_record();
            }
    });
}
void Controller::select(const std::string &id, bool value) {
    command([this, id, value] {
        for (auto &c : channels_)
            if (c->state.source.id == id && !c->removed)
                assign(*c, value);
    });
}
void Controller::assign(Channel &c, bool value) {
    if (value == c.state.source.selected)
        return;
    if (value) {
        for (unsigned i = 0; i < slots_.size(); ++i) {
            auto &s = slots_[i];
            std::lock_guard<std::mutex> l(s.mutex);
            if (s.channel.empty()) {
                c.state.slot = int(i);
                c.state.source.selected = true;
                if (c.state.error == "Maximum 16 displayed channels")
                    c.state.error.clear();
                c.state.generation = ++next_generation_;
                s.channel = c.state.source.id;
                s.generation = c.state.generation;
                s.detections.clear();
                s.latest = {};
                s.error.clear();
                s.last_ai = {};
                c.decode_count_base = s.processed;
                c.ai_count_base = s.inferred;
                return;
            }
        }
        c.state.error = "Maximum 16 displayed channels";
    } else {
        c.stop_decode();
        const int i = c.state.slot;
        c.state.slot = -1;
        c.state.source.selected = false;
        c.state.generation = ++next_generation_;
        if (i >= 0) {
            auto &s = slots_[i];
            std::lock_guard<std::mutex> l(s.mutex);
            s.channel.clear();
            s.generation = c.state.generation;
            s.latest = {};
            s.detections.clear();
        }
    }
}
std::vector<Status> Controller::status() const {
    std::lock_guard<std::mutex> l(mutex_);
    return statuses_;
}
std::string Controller::fatal() const {
    std::lock_guard<std::mutex> l(mutex_);
    return fatal_;
}
void Controller::loop() {
    try {
        bool space = true;
        std::string storage_error;
        auto quota_at = Clock::now();
        auto stats_at = Clock::now();
        std::map<std::string, std::pair<uint64_t, uint64_t>> counts;
        while (true) {
            std::deque<std::function<void()>> work;
            {
                std::lock_guard<std::mutex> l(mutex_);
                work.swap(commands_);
            }
            for (auto &f : work)
                f();
            if (stopping_)
                break;
            if (Clock::now() >= quota_at) {
                try {
                    space = catalog_->enforce(options_.quota, options_.reserve);
                    storage_error.clear();
                } catch (const std::exception &e) {
                    space = false;
                    storage_error = e.what();
                }
                quota_at = Clock::now() + std::chrono::seconds(1);
            }
            for (auto &c : channels_) {
                c->tick(space);
                if (!storage_error.empty() && !c->removed) {
                    c->state.recording = "storage paused";
                    c->state.error = "Storage: " + storage_error;
                }
            }
            channels_.erase(
                std::remove_if(channels_.begin(), channels_.end(),
                               [](const auto &c) { return c->removed && c->recordings.empty(); }),
                channels_.end());
            if (reclaim_pending_ && Clock::now() >= reclaim_at_) {
                malloc_trim(0);
                reclaim_pending_ = false;
                reclaim_at_ = Clock::now() + std::chrono::seconds(5);
            }
            const double elapsed = std::chrono::duration<double>(Clock::now() - stats_at).count();
            std::vector<Status> snapshot;
            for (auto &c : channels_)
                if (!c->removed) {
                    if (elapsed >= 1) {
                        auto &old = counts[c->state.source.id];
                        c->state.display_fps =
                            (c->state.decoded >= old.first ? c->state.decoded - old.first : 0) /
                            elapsed;
                        c->state.ai_fps =
                            (c->state.ai >= old.second ? c->state.ai - old.second : 0) / elapsed;
                        old = {c->state.decoded, c->state.ai};
                    }
                    snapshot.push_back(c->state);
                }
            if (elapsed >= 1) {
                stats_at = Clock::now();
                for (auto it = counts.begin(); it != counts.end();) {
                    if (std::none_of(channels_.begin(), channels_.end(), [&](const auto &c) {
                            return !c->removed && c->state.source.id == it->first;
                        }))
                        it = counts.erase(it);
                    else
                        ++it;
                }
            }
            {
                std::lock_guard<std::mutex> l(mutex_);
                statuses_ = std::move(snapshot);
                if (initial_enqueued_ && commands_.empty())
                    status_ready_ = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (auto &c : channels_) {
            c->stop_receive();
            c->stop_decode();
            c->end_record();
        }
        const auto until = Clock::now() + std::chrono::seconds(5);
        bool pending = true;
        while (pending && Clock::now() < until) {
            pending = false;
            for (auto &c : channels_) {
                c->recordings.erase(std::remove_if(c->recordings.begin(), c->recordings.end(),
                                                   [](auto &r) { return r->poll(); }),
                                    c->recordings.end());
                pending |= !c->recordings.empty();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        {
            std::vector<Status> final;
            for (const auto &c : channels_)
                if (!c->removed) {
                    auto state = c->state;
                    state.connection = "stopped";
                    state.recording = "closed";
                    final.push_back(std::move(state));
                }
            std::lock_guard<std::mutex> lock(mutex_);
            statuses_ = std::move(final);
            if (initial_enqueued_)
                status_ready_ = true;
        }
        channels_.clear();
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> l(mutex_);
        fatal_ = e.what();
    }
}
namespace {
const char *pack_source = R"(
extern "C" __global__ void pack(const unsigned char* bgra,unsigned offset,unsigned pitch,unsigned sw,unsigned sh,unsigned px,unsigned py,float* tensor) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=640*640)return;
 unsigned x=i%640,y=i/640;float r=114.0f/255.0f,g=r,b=r;
 if(x>=px && x<px+sw && y>=py && y<py+sh){const unsigned char* p=bgra+offset+(y-py)*pitch+(x-px)*4;b=p[0]/255.0f;g=p[1]/255.0f;r=p[2]/255.0f;}
 tensor[i]=r;tensor[640*640+i]=g;tensor[2*640*640+i]=b;
}
extern "C" __global__ void pad(float* input,unsigned valid,unsigned batch) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;unsigned size=3*640*640;
 if(i<(batch-valid)*size)input[valid*size+i]=114.0f/255.0f;
})";
}
void Controller::preview_loop() {
    try {
        std::array<Sample, 16> previous;
        while (!stopping_) {
            for (unsigned index = 0; index < slots_.size(); ++index) {
                auto &s = slots_[index];
                std::lock_guard<std::mutex> l(s.mutex);
                if (s.channel.empty() || !s.latest.sample) {
                    previous[index].reset();
                    continue;
                }
                if (s.latest.sample == previous[index])
                    continue;
                auto it = std::find_if(s.preview.begin(), s.preview.end(),
                                       [](const auto &p) { return p.state == 0; });
                if (it == s.preview.end())
                    continue;
                const auto frame = s.latest;
                auto *buffer = gst_sample_get_buffer(frame.sample.get());
                if (gst_va_buffer_peek_display(buffer) != display_)
                    throw std::runtime_error("Decoder did not use the shared VA display");
                const auto surface = gst_va_buffer_get_surface(buffer);
                if (surface == VA_INVALID_SURFACE)
                    throw std::runtime_error("Missing VA decode surface");
                const double factor =
                    std::clamp(std::min(double(std::min(1920u, s.target_width)) / frame.width,
                                        double(std::min(1080u, s.target_height)) / frame.height),
                               1.0 / 6.0, 16.0);
                // Keep VPE within the validated single-stage reduction range even
                // when the desktop window is minimized; GL handles further reduction.
                it->width = std::max(2u, 2 * unsigned(std::ceil(frame.width * factor / 2)));
                it->height = std::max(2u, 2 * unsigned(std::ceil(frame.height * factor / 2)));
                const std::array<unsigned, 4> geometry{frame.width, frame.height, it->width,
                                                       it->height};
                if (!s.preview_va || s.preview_geometry != geometry) {
                    s.preview_va.reset();
                    s.preview_va = std::make_unique<VaSession>(
                        static_cast<VADisplay>(gst_va_display_get_va_dpy(display_)));
                    s.preview_geometry = geometry;
                }
                s.preview_va->scale(surface, it->surface, frame.width, frame.height, it->width,
                                    it->height);
                it->original_width = frame.width;
                it->original_height = frame.height;
                it->generation = frame.generation;
                it->sequence = ++s.sequence;
                it->time = frame.received;
                it->state = 1;
                s.processed++;
                previous[index] = frame.sample;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> l(mutex_);
        fatal_ = std::string("Preview: ") + e.what();
    }
}
void Controller::gpu_loop() {
    try {
        hip_check(hipSetDevice(0), "AI device");
        BatchEngine batch(options_.model, options_.model_info, options_.batch);
        std::unique_ptr<BatchEngine> single;
        if (options_.batch > 1)
            single = std::make_unique<BatchEngine>(options_.single_model, options_.model_info, 1);
        DeviceBuffer input(size_t(options_.batch) * 3 * 640 * 640 * sizeof(float));
        Kernel pack(pack_source, "pack", gpu_arch(0)), pad(pack_source, "pad", gpu_arch(0));
        // Prime both model shapes before enabling the live copy audit and sampling
        // resource baselines; lazy kernels/workspaces must not first appear on
        // a later reconnect's single-image batch.
        {
            void *data = input.data();
            unsigned zero = 0, size = options_.batch;
            void *args[] = {&data, &zero, &size};
            pad.launch(size * 3 * 640 * 640, args);
            hip_check(hipDeviceSynchronize(), "Warmup input");
            batch.infer(data, size);
            if (single)
                single->infer(data, 1);
        }
        inference_ready_ = true;
        std::array<Sample, 16> previous;
        unsigned cursor = 0;
        double rate = 5;
        auto waiting = Clock::time_point{};
        struct Item {
            unsigned slot;
            Frame frame;
            std::string channel;
            unsigned sw, sh, px, py;
        };
        while (!stopping_) {
            std::vector<Item> items;
            const auto now = Clock::now();
            for (unsigned i = 0; i < 16 && items.size() < options_.batch; ++i) {
                const unsigned index = (cursor + i) % 16;
                auto &s = slots_[index];
                std::lock_guard<std::mutex> l(s.mutex);
                if (s.channel.empty() || !s.latest.sample) {
                    previous[index].reset();
                    continue;
                }
                if (s.latest.sample == previous[index] ||
                    std::chrono::duration<double>(now - s.last_ai).count() < 1.0 / rate)
                    continue;
                const auto &f = s.latest;
                const double scale = std::min(640.0 / f.width, 640.0 / f.height);
                const unsigned sw = std::max(1u, unsigned(std::lround(f.width * scale))),
                               sh = std::max(1u, unsigned(std::lround(f.height * scale)));
                items.push_back({index, f, s.channel, sw, sh, (640 - sw) / 2, (640 - sh) / 2});
            }
            if (items.empty()) {
                waiting = {};
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (waiting == Clock::time_point{})
                waiting = now;
            if (items.size() < options_.batch && now - waiting < std::chrono::milliseconds(20)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            waiting = {};
            const auto started = Clock::now();
            unsigned valid = 0;
            std::vector<Item> prepared;
            for (auto &item : items) {
                auto &s = slots_[item.slot];
                std::lock_guard<std::mutex> l(s.mutex);
                if (s.channel != item.channel || s.generation != item.frame.generation)
                    continue;
                auto *buffer = gst_sample_get_buffer(item.frame.sample.get());
                if (gst_va_buffer_peek_display(buffer) != display_)
                    throw std::runtime_error("AI VA display mismatch");
                const std::array<unsigned, 4> geometry{item.frame.width, item.frame.height, item.sw,
                                                       item.sh};
                if (s.ai_geometry[0] && s.ai_geometry != geometry) {
                    s.va.reset();
                    s.va = std::make_unique<VaSession>(
                        static_cast<VADisplay>(gst_va_display_get_va_dpy(display_)));
                }
                s.ai_geometry = geometry;
                s.va->scale(gst_va_buffer_get_surface(buffer), s.ai_surface, item.frame.width,
                            item.frame.height, item.sw, item.sh);
                const auto &layout = s.image->layout();
                const uint8_t *pixels = s.image->data();
                unsigned offset = layout.y_offset, pitch = layout.y_pitch;
                auto *output = static_cast<float *>(input.data()) + size_t(valid) * 3 * 640 * 640;
                void *args[] = {&pixels,  &offset,  &pitch,   &item.sw,
                                &item.sh, &item.px, &item.py, &output};
                pack.launch(640 * 640, args);
                hip_check(hipDeviceSynchronize(), "AI packing completion");
                s.last_ai = Clock::now();
                previous[item.slot] = item.frame.sample;
                prepared.push_back(item);
                ++valid;
            }
            if (!valid)
                continue;
            if (valid > 1 && valid < options_.batch) {
                void *data = input.data();
                unsigned size = options_.batch;
                void *args[] = {&data, &valid, &size};
                pad.launch((size - valid) * 3 * 640 * 640, args);
                hip_check(hipDeviceSynchronize(), "Batch padding completion");
            }
            auto results = (valid == 1 && single ? *single : batch).infer(input.data(), valid);
            const auto completed = Clock::now();
            const double elapsed =
                std::chrono::duration<double, std::milli>(completed - started).count();
            batch_ms_ = elapsed;
            last_batch_ = valid;
            unsigned selected = 0;
            for (auto &s : slots_) {
                std::lock_guard<std::mutex> l(s.mutex);
                if (!s.channel.empty())
                    selected++;
            }
            const double capacity =
                0.75 * valid * 1000 / std::max(1.0, elapsed) / std::max(1u, selected);
            double desired = std::clamp(capacity, 0.1, 30.0);
            for (unsigned i = 0; i < valid; ++i) {
                const auto &item = prepared[i];
                auto &s = slots_[item.slot];
                std::lock_guard<std::mutex> l(s.mutex);
                if (s.channel != item.channel || s.generation != item.frame.generation ||
                    (s.latest.sample &&
                     (s.latest.width != item.frame.width || s.latest.height != item.frame.height)))
                    continue;
                s.detections.clear();
                const float sx = float(item.frame.width) / item.sw,
                            sy = float(item.frame.height) / item.sh;
                for (auto box : results[i]) {
                    const float x = std::clamp((box.x - item.px) * sx, 0.f,
                                               float(item.frame.width)),
                                y = std::clamp((box.y - item.py) * sy, 0.f,
                                               float(item.frame.height));
                    const float right = std::clamp((box.x + box.w - item.px) * sx, 0.f,
                                                   float(item.frame.width)),
                                bottom = std::clamp((box.y + box.h - item.py) * sy, 0.f,
                                                    float(item.frame.height));
                    box.x = x;
                    box.y = y;
                    box.w = right - x;
                    box.h = bottom - y;
                    if (box.w > 0 && box.h > 0)
                        s.detections.push_back(box);
                }
                s.detected_at = item.frame.received;
                s.detection_width = item.frame.width;
                s.detection_height = item.frame.height;
                s.inferred++;
                if (completed - item.frame.received > std::chrono::milliseconds(200))
                    desired = std::min(desired, rate * 0.8);
            }
            rate = std::clamp(rate * 0.8 + desired * 0.2, 0.1, 30.0);
            cursor = (prepared.back().slot + 1) % 16;
        }
        hip_check(hipDeviceSynchronize(), "AI shutdown");
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> l(mutex_);
        fatal_ = std::string("AI: ") + e.what();
    }
}
} // namespace amdva::nvr
