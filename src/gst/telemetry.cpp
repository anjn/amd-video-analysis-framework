#include "amdva/gst_telemetry.hpp"
#include <algorithm>
#include <atomic>
#include <gst/rtp/gstrtpbuffer.h>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>
namespace amdva {
namespace {
gboolean meta_init(GstMeta *meta, gpointer, GstBuffer *) {
    reinterpret_cast<FrameTraceMeta *>(meta)->token = nullptr;
    return TRUE;
}
void meta_free(GstMeta *meta, GstBuffer *) {
    delete reinterpret_cast<FrameTraceMeta *>(meta)->token;
}
gboolean meta_copy(GstBuffer *dest, GstMeta *meta, GstBuffer *, GQuark type, gpointer) {
    if (!GST_META_TRANSFORM_IS_COPY(type))
        return FALSE;
    auto *m = reinterpret_cast<FrameTraceMeta *>(meta);
    try {
        return m->token && set_frame_trace(dest, *m->token);
    } catch (...) {
        return FALSE;
    }
}
const GstMetaInfo *meta_info() {
    static const auto *info =
        gst_meta_register(frame_trace_meta_api(), "AmdFrameTraceMeta", sizeof(FrameTraceMeta),
                          meta_init, meta_free, meta_copy);
    return info;
}
GST_DEBUG_CATEGORY_STATIC(timing_debug);
constexpr size_t limit = 4096;
constexpr int64_t retention = 30000000000LL;
} // namespace
GType frame_trace_meta_api() {
    static const GType type = [] {
        const gchar *tags[] = {nullptr};
        return gst_meta_api_type_register("AmdFrameTraceMetaAPI", tags);
    }();
    return type;
}
FrameTraceMeta *get_frame_trace_meta(GstBuffer *b) {
    return reinterpret_cast<FrameTraceMeta *>(gst_buffer_get_meta(b, frame_trace_meta_api()));
}
TraceToken frame_trace(GstBuffer *b) {
    auto *m = get_frame_trace_meta(b);
    return m && m->token ? *m->token : TraceToken{};
}
bool set_frame_trace(GstBuffer *b, const TraceToken &token) {
    if (!gst_buffer_is_writable(b))
        return false;
    auto *m = get_frame_trace_meta(b);
    // Aggregation must not replace the main branch's identity with another frame.
    if (m && m->token) {
        const auto &old = *m->token;
        if (old.telemetry != token.telemetry || old.frame.generation != token.frame.generation ||
            old.frame.sequence != token.frame.sequence)
            return false;
        *m->token = token;
        return true;
    }
    auto owned = std::make_unique<TraceToken>(token);
    m = reinterpret_cast<FrameTraceMeta *>(gst_buffer_add_meta(b, meta_info(), nullptr));
    if (!m)
        return false;
    m->token = owned.release();
    return true;
}
struct PipelineTelemetry::Impl {
    std::shared_ptr<Telemetry> telemetry;
    std::atomic<bool> background{false};
    std::mutex mutex;
    struct Rtp {
        TraceToken trace;
        int64_t seen{};
    };
    struct Input {
        TraceToken trace;
        int64_t seen{};
        uint64_t rtp{};
        bool ambiguous{}, used{};
    };
    struct Pending {
        TraceToken trace;
        int64_t seen{};
        bool ambiguous{}, entered{};
    };
    std::map<uint64_t, Rtp> rtp;
    std::map<GstClockTime, Input> input;
    std::map<GstClockTime, Pending> pending;
    uint64_t generation{};
    std::array<int64_t, 3> last_prune{};
    std::mutex bindings_mutex;
    struct Probe {
        GstPad *pad;
        gulong id;
    };
    struct Signal {
        GstElement *element;
        gulong id;
    };
    std::vector<Probe> probes;
    std::vector<Signal> signals;
    std::set<std::pair<GstPad *, int>> attached;
    std::atomic<bool> had_au{false};
    enum Mode { RtpIn, RtpOut, Au, DecoderIn, DecoderOut, Stage };
    struct Callback {
        std::shared_ptr<Impl> self;
        Mode mode;
        TimingStage stage;
        bool video{};
    };
    explicit Impl(std::shared_ptr<Telemetry> t) : telemetry(std::move(t)) {
        if (!telemetry)
            throw std::invalid_argument("Missing telemetry collector");
        generation = telemetry->generation();
        static const bool debug = [] {
            GST_DEBUG_CATEGORY_INIT(timing_debug, "amdva-timing", 0, "Frame timing correspondence");
            return true;
        }();
        (void)debug;
    }
    void count(TimingCounter c, uint64_t n = 1) { telemetry->counter(c, n); }
    template <class Map> void prune(Map &map, int64_t now, bool decoder = false) {
        unsigned kind =
            decoder ? 2
                    : (static_cast<const void *>(&map) == static_cast<const void *>(&rtp) ? 0 : 1);
        if (map.size() <= limit && now - last_prune[kind] < 1000000000LL)
            return;
        last_prune[kind] = now;
        for (auto it = map.begin(); it != map.end();)
            if (now - it->second.seen > retention) {
                if (decoder)
                    count(TimingCounter::SubmittedWithoutOutput);
                count(TimingCounter::Expired);
                it = map.erase(it);
            } else
                ++it;
        while (map.size() > limit) {
            auto oldest = map.begin();
            for (auto it = map.begin(); it != map.end(); ++it)
                if (it->second.seen < oldest->second.seen)
                    oldest = it;
            if (decoder)
                count(TimingCounter::SubmittedWithoutOutput);
            map.erase(oldest);
            count(TimingCounter::CapacityEvicted);
        }
    }
    void epoch(const TraceToken &token) {
        if (token.telemetry && generation != token.frame.generation) {
            generation = token.frame.generation;
            rtp.clear();
            input.clear();
            if (!pending.empty())
                count(TimingCounter::SubmittedWithoutOutput, pending.size());
            pending.clear();
        }
    }
    void register_input(GstBuffer *b) {
        auto token = frame_trace(b);
        if (token.telemetry != telemetry)
            return;
        if (token.frame.generation != telemetry->generation()) {
            count(TimingCounter::Stale);
            return;
        }
        token.frame.background = background.load();
        const auto pts = GST_BUFFER_PTS(b);
        if (!GST_CLOCK_TIME_IS_VALID(pts)) {
            count(TimingCounter::AmbiguousPts);
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        epoch(token);
        auto [it, inserted] = pending.emplace(pts, Pending{token, telemetry->now(), false, false});
        if (!inserted && (it->second.trace.frame.sequence != token.frame.sequence ||
                          it->second.trace.frame.generation != token.frame.generation)) {
            if (!it->second.ambiguous)
                count(TimingCounter::AmbiguousPts);
            it->second.ambiguous = true;
        }
        prune(pending, telemetry->now(), true);
    }
    void packet(GstBuffer *b, bool output) {
        GstRTPBuffer packet = GST_RTP_BUFFER_INIT;
        if (!gst_rtp_buffer_map(b, GST_MAP_READ, &packet))
            return;
        const uint64_t key = (uint64_t(gst_rtp_buffer_get_ssrc(&packet)) << 32) |
                             gst_rtp_buffer_get_timestamp(&packet);
        gst_rtp_buffer_unmap(&packet);
        const auto now = telemetry->now();
        const auto gen = telemetry->generation();
        std::lock_guard<std::mutex> lock(mutex);
        if (gen != generation) {
            generation = gen;
            rtp.clear();
            input.clear();
            count(TimingCounter::SubmittedWithoutOutput, pending.size());
            pending.clear();
        }
        if (!output) {
            if (!rtp.count(key))
                rtp.emplace(key, Rtp{{telemetry, telemetry->frame(now, background.load())}, now});
            prune(rtp, now);
            return;
        }
        const auto pts = GST_BUFFER_PTS(b);
        if (!GST_CLOCK_TIME_IS_VALID(pts))
            return;
        auto first = rtp.find(key);
        if (first == rtp.end() || now - first->second.seen > retention)
            return;
        auto [it, inserted] =
            input.emplace(pts, Input{first->second.trace, now, key, false, false});
        if (!inserted && it->second.rtp != key) {
            if (!it->second.ambiguous)
                count(TimingCounter::AmbiguousPts);
            it->second.ambiguous = true;
        }
        prune(input, now);
    }
    GstBuffer *process(GstBuffer *buffer, Mode mode, TimingStage stage) {
        if (mode == RtpIn || mode == RtpOut) {
            packet(buffer, mode == RtpOut);
            return buffer;
        }
        auto token = frame_trace(buffer);
        const auto pts = GST_BUFFER_PTS(buffer);
        const auto now = telemetry->now();
        if (mode == Au) {
            had_au = true;
            if (!token.telemetry) {
                TraceToken matched;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    prune(input, now);
                    auto it = input.find(pts);
                    if (it != input.end() && !it->second.ambiguous && !it->second.used &&
                        now - it->second.seen <= retention) {
                        matched = it->second.trace;
                        it->second.used = true;
                    } else
                        count(TimingCounter::UnmatchedRtp);
                }
                token = matched.telemetry
                            ? matched
                            : TraceToken{telemetry, telemetry->frame(0, background.load())};
            }
            token.mark(TimingStage::AccessUnit, now);
        } else if (mode == DecoderIn) {
            if (token.telemetry)
                register_input(buffer);
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = pending.find(pts);
                if (it != pending.end() && !it->second.ambiguous) {
                    token = it->second.trace;
                    it->second.entered = true;
                } else
                    token = {};
            }
            if (!token.telemetry) {
                count(TimingCounter::UnmatchedDecode);
                token = {telemetry, telemetry->frame(0, background.load())};
            }
            GST_CAT_LOG(timing_debug, "input pts=%" G_GUINT64_FORMAT " frame=%" G_GUINT64_FORMAT,
                        pts, token.frame.sequence);
            token.mark(TimingStage::DecodeInput, now);
            {
                std::lock_guard<std::mutex> lock(mutex);
                auto it = pending.find(pts);
                if (it != pending.end() && !it->second.ambiguous)
                    it->second.trace = token;
            }
        } else if (mode == DecoderOut) {
            GST_CAT_LOG(timing_debug,
                        "output pts=%" G_GUINT64_FORMAT " meta_frame=%" G_GUINT64_FORMAT, pts,
                        token.frame.sequence);
            const bool explicit_identity = token.telemetry == telemetry &&
                                           token.frame.generation == telemetry->generation() &&
                                           token.frame.ns[unsigned(TimingStage::DecodeInput)];
            bool matched = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                prune(pending, now, true);
                auto it = pending.end();
                if (explicit_identity) {
                    it = std::find_if(pending.begin(), pending.end(), [&](const auto &entry) {
                        return !entry.second.ambiguous && entry.second.entered &&
                               entry.second.trace.frame.generation == token.frame.generation &&
                               entry.second.trace.frame.sequence == token.frame.sequence;
                    });
                } else if (GST_CLOCK_TIME_IS_VALID(pts)) {
                    auto by_pts = pending.find(pts);
                    if (by_pts != pending.end() && !by_pts->second.ambiguous &&
                        by_pts->second.entered)
                        it = by_pts;
                }
                if (it != pending.end()) {
                    token = it->second.trace;
                    pending.erase(it);
                    matched = true;
                }
            }
            if (!matched && !(explicit_identity && !token.frame.ns[unsigned(TimingStage::Input)])) {
                count(TimingCounter::UnmatchedDecode);
                token = {telemetry, telemetry->frame(0, background.load())};
            }
            token.mark(TimingStage::DecodeOutput, now);
        } else if (token.telemetry)
            token.mark(stage, now);
        if (!token.telemetry)
            return buffer;
        buffer = gst_buffer_make_writable(buffer);
        if (auto *old = get_frame_trace_meta(buffer);
            old && old->token &&
            (old->token->telemetry != token.telemetry ||
             old->token->frame.generation != token.frame.generation ||
             old->token->frame.sequence != token.frame.sequence))
            gst_buffer_remove_meta(buffer, &old->meta);
        set_frame_trace(buffer, token);
        return buffer;
    }
    static GstPadProbeReturn probe(GstPad *, GstPadProbeInfo *info, gpointer data) {
        auto &stored = *static_cast<Callback *>(data);
        auto cb = stored;
        try {
            if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
                auto *event = GST_PAD_PROBE_INFO_EVENT(info);
                const auto type = GST_EVENT_TYPE(event);
                if (cb.mode == RtpIn && type == GST_EVENT_CAPS) {
                    GstCaps *caps{};
                    gst_event_parse_caps(event, &caps);
                    auto *s = gst_caps_get_structure(caps, 0);
                    const auto *media = gst_structure_get_string(s, "media"),
                               *enc = gst_structure_get_string(s, "encoding-name");
                    stored.video =
                        (media && g_str_equal(media, "video")) ||
                        (!media && enc && (g_str_equal(enc, "H264") || g_str_equal(enc, "H265")));
                }
                if ((cb.mode == Au && type == GST_EVENT_STREAM_START &&
                     cb.self->had_au.exchange(false)) ||
                    ((cb.mode == Au || cb.mode == DecoderIn) && type == GST_EVENT_FLUSH_START)) {
                    cb.self->telemetry->begin_generation();
                    std::lock_guard<std::mutex> lock(cb.self->mutex);
                    cb.self->count(TimingCounter::SubmittedWithoutOutput, cb.self->pending.size());
                    cb.self->pending.clear();
                    cb.self->rtp.clear();
                    cb.self->input.clear();
                    cb.self->generation = cb.self->telemetry->generation();
                }
                return GST_PAD_PROBE_OK;
            }
            if (cb.mode == RtpIn && !cb.video)
                return GST_PAD_PROBE_OK;
            if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
                auto *b = GST_PAD_PROBE_INFO_BUFFER(info);
                GST_PAD_PROBE_INFO_DATA(info) = cb.self->process(b, cb.mode, cb.stage);
            } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
                auto *list = gst_buffer_list_make_writable(GST_PAD_PROBE_INFO_BUFFER_LIST(info));
                GST_PAD_PROBE_INFO_DATA(info) = list;
                gst_buffer_list_foreach(
                    list,
                    [](GstBuffer **buffer, guint, gpointer data) -> gboolean {
                        auto &cb = *static_cast<Callback *>(data);
                        *buffer = cb.self->process(*buffer, cb.mode, cb.stage);
                        return TRUE;
                    },
                    &cb);
            }
        } catch (...) {
            cb.self->count(TimingCounter::InvalidTime);
        }
        return GST_PAD_PROBE_OK;
    }
    void pad(GstPad *pad, Mode mode, TimingStage stage = TimingStage::Input) {
        if (!pad || !telemetry->enabled())
            return;
        std::lock_guard<std::mutex> lock(bindings_mutex);
        if (!attached.emplace(pad, int(mode) * 16 + int(stage)).second)
            return;
        auto *cb = new Callback{shared_from_this(), mode, stage};
        auto id = gst_pad_add_probe(
            pad,
            GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
                            GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH),
            probe, cb, [](gpointer p) { delete static_cast<Callback *>(p); });
        probes.push_back({GST_PAD(gst_object_ref(pad)), id});
    }
    // The owning adapter explicitly detaches callbacks, breaking their shared ownership.
    std::weak_ptr<Impl> weak;
    std::shared_ptr<Impl> shared_from_this() { return weak.lock(); }
    static void manager_pad(GstElement *, GstPad *pad, gpointer data) {
        auto self = *static_cast<std::shared_ptr<Impl> *>(data);
        if (g_str_has_prefix(GST_PAD_NAME(pad), "recv_rtp_sink_"))
            self->pad(pad, RtpIn);
    }
    static void source_pad(GstElement *, GstPad *pad, gpointer data) {
        auto self = *static_cast<std::shared_ptr<Impl> *>(data);
        auto *caps = gst_pad_query_caps(pad, nullptr);
        bool video = false;
        if (caps && !gst_caps_is_empty(caps)) {
            const auto *s = gst_caps_get_structure(caps, 0);
            const char *media = gst_structure_get_string(s, "media");
            video = media && g_str_equal(media, "video");
        }
        if (caps)
            gst_caps_unref(caps);
        if (video)
            self->pad(pad, RtpOut);
    }
    template <class F> void connect(GstElement *element, const char *name, F callback) {
        auto *owned = new std::shared_ptr<Impl>(shared_from_this());
        auto id = g_signal_connect_data(
            element, name, G_CALLBACK(callback), owned,
            [](gpointer p, GClosure *) { delete static_cast<std::shared_ptr<Impl> *>(p); },
            GConnectFlags(0));
        std::lock_guard<std::mutex> lock(bindings_mutex);
        signals.push_back({GST_ELEMENT(gst_object_ref(element)), id});
    }
    static void manager(GstElement *, GstElement *manager, gpointer data) {
        auto self = *static_cast<std::shared_ptr<Impl> *>(data);
        self->connect(manager, "pad-added", manager_pad);
        auto *iterator = gst_element_iterate_sink_pads(manager);
        GValue value = G_VALUE_INIT;
        while (gst_iterator_next(iterator, &value) == GST_ITERATOR_OK) {
            manager_pad(manager, GST_PAD(g_value_get_object(&value)), data);
            g_value_reset(&value);
        }
        g_value_unset(&value);
        gst_iterator_free(iterator);
    }
    void detach() {
        // Pipelines must be stopped first: no application callback races teardown.
        std::vector<Signal> sig;
        std::vector<Probe> prob;
        {
            std::lock_guard<std::mutex> lock(bindings_mutex);
            sig.swap(signals);
            prob.swap(probes);
            attached.clear();
        }
        for (auto s : sig) {
            g_signal_handler_disconnect(s.element, s.id);
            gst_object_unref(s.element);
        }
        for (auto p : prob) {
            gst_pad_remove_probe(p.pad, p.id);
            gst_object_unref(p.pad);
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!pending.empty())
            count(TimingCounter::SubmittedWithoutOutput, pending.size());
        pending.clear();
    }
};
PipelineTelemetry::PipelineTelemetry(std::shared_ptr<Telemetry> t)
    : impl_(std::make_shared<Impl>(std::move(t))) {
    impl_->weak = impl_;
}
PipelineTelemetry::~PipelineTelemetry() { impl_->detach(); }
void PipelineTelemetry::attach_rtsp(GstElement *source) {
    if (!impl_->telemetry->enabled())
        return;
    impl_->telemetry->availability(TimingStage::Input, true, true);
    impl_->connect(source, "new-manager", Impl::manager);
    impl_->connect(source, "pad-added", Impl::source_pad);
}
void PipelineTelemetry::attach_rtp(GstPad *in, GstPad *out) {
    impl_->telemetry->availability(TimingStage::Input, true, true);
    impl_->pad(in, Impl::RtpIn);
    impl_->pad(out, Impl::RtpOut);
}
void PipelineTelemetry::attach_access_units(GstPad *pad) {
    impl_->telemetry->availability(TimingStage::AccessUnit, true, true);
    impl_->pad(pad, Impl::Au);
}
void PipelineTelemetry::attach_decoder(GstElement *decoder) {
    for (auto p : {std::pair<const char *, Impl::Mode>{"sink", Impl::DecoderIn},
                   {"src", Impl::DecoderOut}}) {
        auto *pad = gst_element_get_static_pad(decoder, p.first);
        if (!pad)
            throw std::invalid_argument("Decoder must have sink/src pads");
        impl_->pad(pad, p.second);
        gst_object_unref(pad);
    }
    impl_->telemetry->availability(TimingStage::DecodeInput, true, true);
    impl_->telemetry->availability(TimingStage::DecodeOutput, true, true);
}
void PipelineTelemetry::attach_stage(GstPad *pad, TimingStage stage) {
    impl_->telemetry->availability(stage, true, true);
    impl_->pad(pad, Impl::Stage, stage);
}
void PipelineTelemetry::submit(GstBuffer *b) {
    if (impl_->telemetry->enabled())
        impl_->register_input(b);
}
void PipelineTelemetry::background(bool value) { impl_->background = value; }
} // namespace amdva
