#include "amdva/streaming/playback.hpp"
#include <algorithm>
#include <condition_variable>
#include <gst/app/gstappsink.h>
#include <gst/va/gstva.h>
#include <gst/video/video.h>
namespace amdva::streaming {
struct PlaybackSession::Impl {
    std::shared_ptr<GstVaDisplay> display;
    Slot slot;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> commands;
    PlaybackStatus status;
    std::thread worker;
    bool closing{}, playing{}, preroll{}, eos{};
    GstElement *pipeline{}, *sink{};
    std::shared_ptr<RecordingFile> file;
    Sample held;
    std::atomic<bool> linked{false}, unsupported{false};
    Impl(GstVaDisplay *d)
        : display(GST_VA_DISPLAY(gst_object_ref(d)), [](auto *p) { gst_object_unref(p); }) {
        slot.va = std::make_unique<VaSession>(static_cast<VADisplay>(gst_va_display_get_va_dpy(d)));
        for (auto &p : slot.preview)
            p.surface = slot.va->create_bgra(1920, 1080);
        worker = std::thread([this] { loop(); });
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> l(mutex);
            closing = true;
        }
        wake.notify_all();
        worker.join();
    }
    void enqueue(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> l(mutex);
            commands.push_back(std::move(f));
        }
        wake.notify_one();
    }
    void set_state(const std::string &s) {
        std::lock_guard<std::mutex> l(mutex);
        status.state = s;
    }
    void retire() {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        if (sink) {
            gst_object_unref(sink);
            sink = nullptr;
        }
        held.reset();
        file.reset();
        playing = false;
        preroll = false;
        eos = false;
        {
            std::lock_guard<std::mutex> l(slot.mutex);
            slot.channel.clear();
            slot.generation++;
        }
    }
    static void pad_added(GstElement *, GstPad *pad, gpointer data) {
        auto &s = *static_cast<Impl *>(data);
        auto *caps = gst_pad_get_current_caps(pad);
        if (!caps)
            return;
        const char *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
        const bool h264 = g_str_equal(name, "video/x-h264"),
                   h265 = g_str_equal(name, "video/x-h265");
        if (g_str_has_prefix(name, "video/") && !h264 && !h265)
            s.unsupported = true;
        gst_caps_unref(caps);
        if ((!h264 && !h265) || s.linked.exchange(true))
            return;
        GError *error{};
        auto *branch = gst_parse_bin_from_description(
            h264 ? "h264parse ! vah264dec ! capsfilter "
                   "caps=\"video/x-raw(memory:VAMemory),format=NV12\""
                 : "h265parse ! vah265dec ! capsfilter "
                   "caps=\"video/x-raw(memory:VAMemory),format=NV12\"",
            TRUE, &error);
        if (!branch || error) {
            g_clear_error(&error);
            if (branch)
                gst_object_unref(branch);
            s.unsupported = true;
            return;
        }
        gst_bin_add(GST_BIN(s.pipeline), branch);
        auto *input = gst_element_get_static_pad(branch, "sink");
        bool ok = gst_pad_link(pad, input) == GST_PAD_LINK_OK;
        gst_object_unref(input);
        ok = ok && gst_element_link(branch, s.sink);
        if (!ok || !gst_element_sync_state_with_parent(branch))
            s.unsupported = true;
    }
    void load(std::shared_ptr<RecordingFile> f, bool play) {
        retire();
        {
            std::lock_guard<std::mutex> l(mutex);
            status = {};
            status.recording = f->recording().name;
            status.state = "opening";
        }
        file = std::move(f);
        linked = false;
        unsupported = false;
        pipeline = gst_pipeline_new(nullptr);
        auto *src = gst_element_factory_make("fdsrc", nullptr),
             *demux = gst_element_factory_make("matroskademux", nullptr);
        auto *output = gst_element_factory_make("appsink", nullptr);
        if (!src || !demux || !output) {
            for (auto *p : {src, demux, output})
                if (p)
                    gst_object_unref(p);
            throw std::runtime_error("Playback plugins unavailable");
        }
        sink = GST_ELEMENT(gst_object_ref(output));
        g_object_set(src, "fd", file->fd(), nullptr);
        g_object_set(sink, "sync", TRUE, "max-buffers", 1u, "drop", FALSE, "wait-on-eos", FALSE,
                     nullptr);
        gst_bin_add_many(GST_BIN(pipeline), src, demux, output, nullptr);
        if (!gst_element_link(src, demux))
            throw std::runtime_error("Cannot link recording input");
        auto *context = gst_context_new(GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR, TRUE);
        gst_context_set_va_display(context, display.get());
        gst_element_set_context(pipeline, context);
        gst_context_unref(context);
        auto *pad = gst_element_get_static_pad(sink, "sink");
        gst_pad_add_probe(
            pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
            [](GstPad *, GstPadProbeInfo *i, gpointer) {
                auto *q = GST_PAD_PROBE_INFO_QUERY(i);
                if (GST_QUERY_TYPE(q) != GST_QUERY_ALLOCATION)
                    return GST_PAD_PROBE_OK;
                gst_query_add_allocation_meta(q, GST_VIDEO_META_API_TYPE, nullptr);
                gst_query_add_allocation_meta(q, GST_VIDEO_CROP_META_API_TYPE, nullptr);
                return GST_PAD_PROBE_HANDLED;
            },
            nullptr, nullptr);
        gst_object_unref(pad);
        g_signal_connect(demux, "pad-added", G_CALLBACK(pad_added), this);
        {
            std::lock_guard<std::mutex> l(slot.mutex);
            slot.channel = file->recording().name;
            slot.generation++;
        }
        playing = play;
        preroll = !play;
        if (gst_element_set_state(pipeline, play ? GST_STATE_PLAYING : GST_STATE_PAUSED) ==
            GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Cannot start recording decoder");
    }
    void loop() {
        for (;;) {
            try {
                std::deque<std::function<void()>> work;
                {
                    std::unique_lock<std::mutex> l(mutex);
                    if (!pipeline && commands.empty() && !closing)
                        wake.wait(l, [this] { return closing || !commands.empty(); });
                    if (closing)
                        break;
                    work.swap(commands);
                }
                for (auto &f : work)
                    f();
                if (!pipeline)
                    continue;
                if (unsupported)
                    throw std::runtime_error("Unsupported recording codec or playback negotiation");
                auto *bus = gst_element_get_bus(pipeline);
                bool ended = false, failed = false;
                while (auto *m = gst_bus_pop(bus)) {
                    failed |= GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR;
                    ended |= GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
                    gst_message_unref(m);
                }
                gst_object_unref(bus);
                if (failed)
                    throw std::runtime_error("Cannot read or decode recording");
                if (ended)
                    eos = true;
                auto *sample =
                    held      ? held.get()
                    : playing ? gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_MSECOND)
                    : preroll ? gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 10 * GST_MSECOND)
                              : nullptr;
                if (sample) {
                    if (!held)
                        held = Sample(sample, [](auto *p) { gst_sample_unref(p); });
                    bool rendered = false;
                    {
                        std::lock_guard<std::mutex> l(slot.mutex);
                        auto it = std::find_if(slot.preview.begin(), slot.preview.end(),
                                               [](auto &p) { return p.state == 0; });
                        if (it != slot.preview.end()) {
                            prepare_preview(display.get(), slot, *it, sample, slot.generation,
                                            ++slot.sequence);
                            it->time = Clock::now();
                            it->state = 1;
                            rendered = true;
                        }
                    }
                    if (rendered) {
                        held.reset();
                        preroll = false;
                    }
                    {
                        std::lock_guard<std::mutex> l(mutex);
                        if (rendered)
                            status.frames++;
                        if (playing || preroll)
                            status.state = playing ? "playing" : "paused";
                    }
                }
                if (eos && !held && gst_app_sink_is_eos(GST_APP_SINK(sink))) {
                    playing = false;
                    set_state("ended");
                }
                gint64 position{}, duration = -1;
                gst_element_query_position(pipeline, GST_FORMAT_TIME, &position);
                gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration);
                {
                    std::lock_guard<std::mutex> l(mutex);
                    status.position_ns = position;
                    status.duration_ns = duration;
                }
                if (!playing && !preroll) {
                    std::unique_lock<std::mutex> l(mutex);
                    wake.wait_for(l, std::chrono::milliseconds(20),
                                  [this] { return closing || !commands.empty(); });
                }
            } catch (const std::exception &e) {
                retire();
                std::lock_guard<std::mutex> l(mutex);
                status.state = "error";
                status.error = e.what();
            }
        }
        retire();
    }
};
PlaybackSession::PlaybackSession(GstVaDisplay *d) {
    if (!d)
        throw std::invalid_argument("Missing playback VA display");
    impl_ = std::make_unique<Impl>(d);
}
PlaybackSession::~PlaybackSession() = default;
void PlaybackSession::open(std::shared_ptr<RecordingFile> f, bool play) {
    if (!f)
        throw std::invalid_argument("Missing recording");
    impl_->enqueue([s = impl_.get(), f = std::move(f), play] { s->load(f, play); });
}
void PlaybackSession::play() {
    impl_->enqueue([s = impl_.get()] {
        if (s->pipeline) {
            s->playing = true;
            s->preroll = false;
            if (gst_element_set_state(s->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
                throw std::runtime_error("Cannot resume playback");
            s->set_state("playing");
        }
    });
}
void PlaybackSession::pause() {
    impl_->enqueue([s = impl_.get()] {
        if (s->pipeline) {
            s->playing = false;
            gst_element_set_state(s->pipeline, GST_STATE_PAUSED);
            s->set_state("paused");
        }
    });
}
void PlaybackSession::seek(int64_t ns) {
    impl_->enqueue([s = impl_.get(), ns] {
        if (s->pipeline) {
            s->held.reset();
            s->eos = false;
            if (!gst_element_seek_simple(s->pipeline, GST_FORMAT_TIME,
                                         GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                                         std::max<int64_t>(0, ns)))
                throw std::runtime_error("Recording is not seekable");
            {
                std::lock_guard<std::mutex> l(s->slot.mutex);
                s->slot.generation++;
            }
            s->preroll = !s->playing;
        }
    });
}
void PlaybackSession::stop() {
    impl_->enqueue([s = impl_.get()] {
        s->retire();
        s->set_state("stopped");
    });
}
PlaybackStatus PlaybackSession::status() const {
    std::lock_guard<std::mutex> l(impl_->mutex);
    return impl_->status;
}
Slot &PlaybackSession::slot() { return impl_->slot; }
} // namespace amdva::streaming
