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
    Slot *target;
    bool external{}, enabled{true}, preparing{};
    uint64_t output_generation{};
    int64_t initial_seek{-1};
    Clock::time_point preparation_deadline;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> commands;
    PlaybackStatus status;
    bool ready{};
    std::thread worker;
    bool closing{}, playing{}, preroll{}, eos{};
    GstElement *pipeline{}, *sink{};
    std::shared_ptr<RecordingFile> file;
    Sample held;
    std::atomic<bool> linked{false}, unsupported{false};
    Impl(GstVaDisplay *d, Slot *output = nullptr)
        : display(GST_VA_DISPLAY(gst_object_ref(d)), [](auto *p) { gst_object_unref(p); }),
          target(output ? output : &slot), external(output != nullptr), enabled(!output) {
        if (!external) {
            slot.va =
                std::make_unique<VaSession>(static_cast<VADisplay>(gst_va_display_get_va_dpy(d)));
            for (auto &p : slot.preview)
                p.surface = slot.va->create_bgra(1920, 1080);
        }
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
        preparing = false;
        if (!external) {
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
    void load(std::shared_ptr<RecordingFile> f, bool play, int64_t at) {
        retire();
        {
            std::lock_guard<std::mutex> l(mutex);
            status = {};
            ready = false;
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
        if (!external) {
            std::lock_guard<std::mutex> l(slot.mutex);
            slot.channel = file->recording().name;
            slot.generation++;
        }
        playing = play;
        preroll = true;
        preparing = true;
        preparation_deadline = Clock::now() + std::chrono::seconds(5);
        initial_seek = at > 0 ? at : -1;
        if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
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
                if (preparing) {
                    if (Clock::now() > preparation_deadline)
                        throw std::runtime_error("Recording preparation timed out");
                    auto state = gst_element_get_state(pipeline, nullptr, nullptr, 0);
                    if (state == GST_STATE_CHANGE_FAILURE)
                        throw std::runtime_error("Cannot prepare recording decoder");
                    if (state != GST_STATE_CHANGE_SUCCESS) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        continue;
                    }
                    if (initial_seek >= 0) {
                        if (!gst_element_seek_simple(
                                pipeline, GST_FORMAT_TIME,
                                GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                                initial_seek))
                            throw std::runtime_error("Recording is not seekable");
                        initial_seek = -1;
                        continue;
                    }
                    preparing = false;
                    if (playing && gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
                                       GST_STATE_CHANGE_FAILURE)
                        throw std::runtime_error("Cannot start prepared recording");
                }
                auto *sample =
                    held      ? held.get()
                    : playing ? gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_MSECOND)
                    : preroll ? gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 10 * GST_MSECOND)
                              : nullptr;
                if (sample) {
                    if (!held)
                        held = Sample(sample, [](auto *p) { gst_sample_unref(p); });
                    bool rendered = false;
                    if (enabled) {
                        auto &out = *target;
                        std::lock_guard<std::mutex> l(out.mutex);
                        auto it = std::find_if(out.preview.begin(), out.preview.end(),
                                               [](auto &p) { return p.state == 0; });
                        if (it != out.preview.end() &&
                            (!external || out.generation == output_generation)) {
                            prepare_preview(display.get(), out, *it, sample, out.generation,
                                            ++out.sequence);
                            it->time = Clock::now();
                            it->state = 1;
                            rendered = true;
                        }
                    }
                    const auto pts = GST_BUFFER_PTS(gst_sample_get_buffer(sample));
                    const auto *segment = gst_sample_get_segment(sample);
                    const auto frame_position =
                        segment && GST_CLOCK_TIME_IS_VALID(pts)
                            ? gst_segment_to_stream_time(segment, GST_FORMAT_TIME, pts)
                            : GST_CLOCK_TIME_NONE;
                    if (rendered) {
                        held.reset();
                        preroll = false;
                    }
                    {
                        std::lock_guard<std::mutex> l(mutex);
                        ready = true;
                        if (rendered) {
                            status.frames++;
                            if (GST_CLOCK_TIME_IS_VALID(frame_position))
                                status.position_ns = frame_position;
                        }
                        if (playing || preroll)
                            status.state = playing ? "playing" : "paused";
                    }
                    if (!enabled)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
                    if (!status.frames)
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
PlaybackSession::PlaybackSession(GstVaDisplay *d, Slot *target) {
    if (!d || !target)
        throw std::invalid_argument("Missing playback display or output");
    impl_ = std::make_unique<Impl>(d, target);
}
void PlaybackSession::output(bool enabled, uint64_t generation) {
    impl_->enqueue([s = impl_.get(), enabled, generation] {
        s->enabled = enabled;
        s->output_generation = generation;
    });
}
bool PlaybackSession::ready() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->ready;
}
void PlaybackSession::open(std::shared_ptr<RecordingFile> f, bool play) {
    open(std::move(f), play, 0);
}
void PlaybackSession::open(std::shared_ptr<RecordingFile> f, bool play, int64_t at) {
    if (!f)
        throw std::invalid_argument("Missing recording");
    impl_->enqueue([s = impl_.get(), f = std::move(f), play, at] { s->load(f, play, at); });
}
void PlaybackSession::play() {
    impl_->enqueue([s = impl_.get()] {
        if (s->pipeline) {
            s->playing = true;
            s->preroll = false;
            if (!s->preparing &&
                gst_element_set_state(s->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
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
            gst_element_set_state(s->pipeline, GST_STATE_PAUSED);
            s->preparing = true;
            s->preparation_deadline = Clock::now() + std::chrono::seconds(5);
            s->initial_seek = std::max<int64_t>(0, ns);
            if (!s->external) {
                std::lock_guard<std::mutex> l(s->slot.mutex);
                s->slot.generation++;
            }
            s->preroll = true;
            {
                std::lock_guard<std::mutex> l(s->mutex);
                s->ready = false;
            }
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

namespace amdva::streaming {
struct TimelinePlaybackSession::Impl {
    // Keep the VA display alive until after the shared output surfaces retire.
    std::shared_ptr<GstVaDisplay> display_owner;
    Catalog &catalog;
    Slot slot;
    std::unique_ptr<PlaybackSession> decoders[2];
    std::optional<Recording> records[2], cursor;
    uint64_t seen[2]{};
    std::set<std::string> rejected;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> commands;
    TimelinePlaybackStatus status;
    std::thread worker;
    bool closing{}, wanted{}, following{true}, selected{}, activated{};
    int active{-1}, prepared{-1};
    int64_t requested{}, next_poll{};
    uint64_t generation{};
    Impl(GstVaDisplay *display, Catalog &c) : catalog(c) {
        if (!display)
            throw std::invalid_argument("Missing playback display");
        display_owner = {GST_VA_DISPLAY(gst_object_ref(display)),
                         [](auto *p) { gst_object_unref(p); }};
        slot.va =
            std::make_unique<VaSession>(static_cast<VADisplay>(gst_va_display_get_va_dpy(display)));
        for (auto &p : slot.preview)
            p.surface = slot.va->create_bgra(1920, 1080);
        for (auto &decoder : decoders)
            decoder.reset(new PlaybackSession(display, &slot));
        worker = std::thread([this] { loop(); });
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
        }
        wake.notify_one();
        worker.join();
        for (auto &decoder : decoders)
            decoder.reset();
    }
    void enqueue(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            commands.push_back(std::move(f));
        }
        wake.notify_one();
    }
    void set_state(const std::string &state) {
        std::lock_guard<std::mutex> lock(mutex);
        status.state = state;
    }
    void clear(bool image) {
        active = prepared = -1;
        for (unsigned i = 0; i < 2; i++) {
            decoders[i]->output(false, 0);
            decoders[i]->stop();
            records[i].reset();
            seen[i] = 0;
        }
        if (image) {
            std::lock_guard<std::mutex> lock(slot.mutex);
            generation = ++slot.generation;
            slot.channel.clear();
        }
    }
    void locate(const std::string &channel, int64_t at, bool play, bool follow) {
        clear(true);
        selected = !channel.empty();
        wanted = play;
        following = follow;
        activated = false;
        cursor.reset();
        rejected.clear();
        requested = at;
        next_poll = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = {};
            status.channel = channel;
            status.requested_ms = at;
            status.position_ms = at;
            status.state = selected ? "opening" : "stopped";
        }
        {
            std::lock_guard<std::mutex> lock(slot.mutex);
            slot.channel = channel;
        }
    }
    int64_t next_time() const {
        return active >= 0 && records[active]
                   ? std::max(requested,
                              records[active]->started_ms +
                                  (records[active]->duration_ns + GST_MSECOND - 1) / GST_MSECOND)
                   : requested;
    }
    bool prepare(Recording r, int index) {
        if (rejected.count(r.name))
            return false;
        try {
            auto f = catalog.open_recording(r.name);
            records[index] = r;
            seen[index] = 0;
            decoders[index]->output(false, 0);
            decoders[index]->open(std::move(f), false,
                                  std::max<int64_t>(0, next_time() - r.started_ms) * GST_MSECOND);
            prepared = index;
            return true;
        } catch (...) {
            reject(r, "Recording became unavailable; skipping to the next recording.");
            return false;
        }
    }
    void reject(const Recording &r, const std::string &message) {
        if (rejected.insert(r.name).second) {
            std::lock_guard<std::mutex> lock(mutex);
            status.skipped++;
            status.notice = message;
        }
    }
    void find() {
        std::string channel;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel = status.channel;
        }
        auto r = cursor ? catalog.next(*cursor) : catalog.resolve(channel, requested);
        // Bound individual iterations even for large runs of corrupt files.
        for (unsigned n = 0; r && n < 32; ++n) {
            if (r->started_ms + (r->duration_ns + GST_MSECOND - 1) / GST_MSECOND > next_time() &&
                !rejected.count(r->name) && prepare(*r, active == 0 ? 1 : 0))
                return;
            cursor = r;
            r = catalog.next(*r);
        }
    }
    void tick() {
        if (!selected)
            return;
        if (active >= 0) {
            auto s = decoders[active]->status();
            if (records[active] && s.recording == records[active]->name) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (s.frames >= seen[active])
                        status.frames += s.frames - seen[active];
                    seen[active] = s.frames;
                    if (s.frames)
                        status.position_ms =
                            records[active]->started_ms +
                            std::min(s.position_ns, records[active]->duration_ns) / GST_MSECOND;
                }
                if (s.state == "error" || s.state == "ended") {
                    if (s.state == "error")
                        reject(*records[active],
                               "Cannot decode recording; skipping to the next recording.");
                    cursor = records[active];
                    requested =
                        cursor->started_ms + (cursor->duration_ns + GST_MSECOND - 1) / GST_MSECOND;
                    decoders[active]->output(false, 0);
                    decoders[active]->stop();
                    records[active].reset();
                    active = -1;
                    next_poll = 0;
                }
            }
        }
        if (prepared >= 0) {
            auto s = decoders[prepared]->status();
            const bool ready = decoders[prepared]->ready();
            if (records[prepared] && s.recording == records[prepared]->name) {
                if (s.state == "error" || (s.state == "ended" && !ready)) {
                    auto bad = *records[prepared];
                    reject(bad, "Cannot decode recording; skipping to the next recording.");
                    decoders[prepared]->stop();
                    records[prepared].reset();
                    prepared = -1;
                    // While playing, advance the lookahead cursor past a bad file.
                    cursor = bad;
                    next_poll = 0;
                } else if (active < 0 && ready) {
                    active = prepared;
                    prepared = -1;
                    auto &r = *records[active];
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (activated)
                            status.transitions++;
                        if (r.started_ms > requested)
                            status.notice = "Recording gap skipped to the next available time.";
                        status.position_ms = std::max(requested, r.started_ms);
                        status.state = wanted ? "playing" : "paused";
                    }
                    activated = true;
                    decoders[active]->output(true, generation);
                    if (wanted)
                        decoders[active]->play();
                    cursor = r;
                    next_poll = 0;
                }
            }
        }
        const auto now = g_get_monotonic_time();
        if (prepared < 0 && now >= next_poll && (active >= 0 || wanted || !activated)) {
            next_poll = now + 1000000;
            find();
        }
        if (active < 0) {
            if (prepared >= 0)
                set_state("opening");
            else {
                set_state(wanted ? (following ? "waiting" : "ended") : "paused");
                if (wanted && !following)
                    selected = false;
            }
        }
    }
    void loop() {
        for (;;) {
            std::deque<std::function<void()>> work;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(5),
                              [&] { return closing || !commands.empty(); });
                if (closing)
                    break;
                work.swap(commands);
            }
            try {
                for (auto &f : work)
                    f();
                tick();
            } catch (const std::exception &e) {
                clear(false);
                selected = false;
                std::lock_guard<std::mutex> lock(mutex);
                status.state = "error";
                status.error = e.what();
            }
        }
        clear(false);
    }
};
TimelinePlaybackSession::TimelinePlaybackSession(GstVaDisplay *d, Catalog &c)
    : impl_(std::make_unique<Impl>(d, c)) {}
TimelinePlaybackSession::~TimelinePlaybackSession() = default;
void TimelinePlaybackSession::open(const std::string &channel, int64_t at, bool play, bool follow) {
    impl_->enqueue(
        [s = impl_.get(), channel, at, play, follow] { s->locate(channel, at, play, follow); });
}
void TimelinePlaybackSession::seek(int64_t at) {
    impl_->enqueue([s = impl_.get(), at] {
        std::string channel;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            channel = s->status.channel;
        }
        s->locate(channel, at, s->wanted, s->following);
    });
}
void TimelinePlaybackSession::play() {
    impl_->enqueue([s = impl_.get()] {
        s->wanted = true;
        if (s->active >= 0) {
            s->decoders[s->active]->play();
            s->set_state("playing");
        }
        s->next_poll = 0;
    });
}
void TimelinePlaybackSession::pause() {
    impl_->enqueue([s = impl_.get()] {
        if (!s->selected)
            return;
        s->wanted = false;
        if (s->active >= 0)
            s->decoders[s->active]->pause();
        s->set_state("paused");
    });
}
void TimelinePlaybackSession::stop() {
    impl_->enqueue([s = impl_.get()] {
        s->clear(true);
        s->selected = false;
        s->wanted = false;
        std::lock_guard<std::mutex> lock(s->mutex);
        s->status.state = "stopped";
        s->status.notice.clear();
        s->status.error.clear();
    });
}
void TimelinePlaybackSession::follow(bool expected) {
    impl_->enqueue([s = impl_.get(), expected] { s->following = expected; });
}
TimelinePlaybackStatus TimelinePlaybackSession::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status;
}
Slot &TimelinePlaybackSession::slot() { return impl_->slot; }
} // namespace amdva::streaming
