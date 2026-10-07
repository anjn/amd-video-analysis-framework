#include "amdva/gst_telemetry.hpp"
#include <atomic>
#include <chrono>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace amdva;
void check(bool v, const char *s) {
    if (!v)
        throw std::runtime_error(s);
}
GstPad *pad(GstElement *pipe, const char *element, const char *name) {
    auto *e = gst_bin_get_by_name(GST_BIN(pipe), element);
    check(e, "Missing element");
    auto *p = gst_element_get_static_pad(e, name);
    gst_object_unref(e);
    return p;
}
struct Reorder {
    std::atomic<int64_t> *clock;
    GstPad *output;
    std::vector<GstBuffer *> queued;
    std::vector<TraceToken> results;
    bool discard_meta{}, rewrite_pts{};
};
GstFlowReturn reorder(GstPad *pad, GstObject *, GstBuffer *buffer) {
    auto &s = *static_cast<Reorder *>(gst_pad_get_element_private(pad));
    s.queued.push_back(buffer);
    if (s.queued.size() == 3) {
        *s.clock += 20000000;
        auto frames = std::move(s.queued);
        s.queued.clear();
        for (auto index : {1, 0, 2}) {
            if (s.discard_meta)
                if (auto *meta = get_frame_trace_meta(frames[index]))
                    gst_buffer_remove_meta(frames[index], &meta->meta);
            if (s.rewrite_pts && index == 1)
                GST_BUFFER_PTS(frames[index]) = 7 * GST_SECOND;
            auto flow = gst_pad_push(s.output, frames[index]);
            if (flow != GST_FLOW_OK)
                return flow;
        }
    }
    return GST_FLOW_OK;
}
GstFlowReturn capture(GstPad *pad, GstObject *, GstBuffer *buffer) {
    auto &s = *static_cast<Reorder *>(gst_pad_get_element_private(pad));
    s.results.push_back(frame_trace(buffer));
    gst_buffer_unref(buffer);
    return GST_FLOW_OK;
}
void reorder_test(std::atomic<int64_t> &at) {
    auto metrics =
        std::make_shared<Telemetry>("reorder", TelemetryOptions{}, [&] { return at.load(); });
    metrics->begin_generation(1);
    auto *decoder = gst_bin_new(nullptr), *receiver = gst_bin_new(nullptr);
    auto *in = gst_pad_new("sink", GST_PAD_SINK), *out = gst_pad_new("src", GST_PAD_SRC),
         *sink = gst_pad_new("sink", GST_PAD_SINK);
    gst_element_add_pad(decoder, in);
    gst_element_add_pad(decoder, out);
    gst_element_add_pad(receiver, sink);
    Reorder state{&at, out, {}, {}};
    state.rewrite_pts = true;
    gst_pad_set_element_private(in, &state);
    gst_pad_set_element_private(sink, &state);
    gst_pad_set_chain_function(in, reorder);
    gst_pad_set_chain_function(sink, capture);
    gst_pad_set_event_function(in, [](GstPad *p, GstObject *, GstEvent *e) {
        auto &s = *static_cast<Reorder *>(gst_pad_get_element_private(p));
        return gst_pad_push_event(s.output, e);
    });
    for (auto *p : {in, out, sink})
        gst_pad_set_active(p, TRUE);
    check(gst_pad_link(out, sink) == GST_PAD_LINK_OK, "Reorder pad link");
    auto adapter = std::make_unique<PipelineTelemetry>(metrics);
    adapter->attach_decoder(decoder);
    gst_pad_send_event(in, gst_event_new_stream_start("reorder"));
    auto *caps = gst_caps_new_empty_simple("video/x-h264");
    gst_pad_send_event(in, gst_event_new_caps(caps));
    gst_caps_unref(caps);
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    gst_pad_send_event(in, gst_event_new_segment(&segment));
    for (unsigned i = 0; i < 3; i++) {
        at += 10000000;
        auto *b = gst_buffer_new();
        GST_BUFFER_PTS(b) = i * GST_SECOND / 30;
        TraceToken token{metrics, metrics->frame(at.load() - 100000000)};
        token.mark(TimingStage::AccessUnit, at.load());
        set_frame_trace(b, token);
        adapter->submit(b);
        check(gst_pad_chain(in, b) == GST_FLOW_OK, "Reorder input");
    }
    check(state.results.size() == 3, "Reorder count");
    check(state.results[0].frame.sequence == 2 && state.results[1].frame.sequence == 1 &&
              state.results[2].frame.sequence == 3,
          "PTS matching instead of FIFO");
    check(metrics->snapshot().latency[0][1].samples == 3, "Reorder latency samples");
    state.rewrite_pts = false;
    state.discard_meta = true;
    // Duplicate PTS: both outputs must have unknown origins, never a guessed input latency.
    for (unsigned i = 0; i < 3; i++) {
        at += 10000000;
        auto *b = gst_buffer_new();
        GST_BUFFER_PTS(b) = GST_SECOND;
        TraceToken token{metrics, metrics->frame(at.load() - 100000000)};
        token.mark(TimingStage::AccessUnit);
        set_frame_trace(b, token);
        adapter->submit(b);
        check(gst_pad_chain(in, b) == GST_FLOW_OK, "Ambiguous input");
    }
    for (unsigned i = 3; i < 6; i++)
        check(state.results[i].frame.ns[0] == 0, "Ambiguous PTS not guessed");
    auto before = metrics->generation();
    gst_pad_send_event(in, gst_event_new_flush_start());
    gst_pad_send_event(in, gst_event_new_flush_stop(TRUE));
    check(metrics->generation() > before, "Flush retires generation");
    auto stale = state.results[0];
    check(!stale.mark(TimingStage::Draw), "Stale after flush");
    for (auto *p : {in, out, sink})
        gst_pad_set_active(p, FALSE);
    adapter.reset();
    gst_object_unref(decoder);
    gst_object_unref(receiver);
}

void input_only_test(bool audio) {
    auto metrics = std::make_shared<Telemetry>(audio ? "audio" : "partial-video");
    metrics->begin_generation(1);
    const auto description =
        std::string("appsrc name=input is-live=true format=time caps=\"application/x-rtp,media=") +
        (audio ? "audio,encoding-name=PCMA,clock-rate=8000,payload=8"
               : "video,encoding-name=H264,clock-rate=90000,payload=96") +
        "\" ! identity name=before ! valve drop=true ! identity name=after ! fakesink sync=false "
        "async=false";
    auto *pipeline = gst_parse_launch(description.c_str(), nullptr),
         *input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    auto *before = pad(pipeline, "before", "sink"), *after = pad(pipeline, "after", "src");
    auto adapter = std::make_unique<PipelineTelemetry>(metrics);
    adapter->attach_rtp(before, after);
    std::atomic<unsigned> delivered{0};
    gst_pad_add_probe(
        before, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *, gpointer p) {
            ++*static_cast<std::atomic<unsigned> *>(p);
            return GST_PAD_PROBE_OK;
        },
        &delivered, nullptr);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    for (unsigned i = 0; i < 8; i++) {
        auto *b = gst_rtp_buffer_new_allocate(4, 0, 0);
        GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
        gst_rtp_buffer_map(b, GST_MAP_WRITE, &rtp);
        gst_rtp_buffer_set_ssrc(&rtp, 43);
        gst_rtp_buffer_set_timestamp(&rtp, 90000);
        gst_rtp_buffer_set_seq(&rtp, 100 + i);
        gst_rtp_buffer_unmap(&rtp);
        GST_BUFFER_PTS(b) = 0;
        check(gst_app_src_push_buffer(GST_APP_SRC(input), b) == GST_FLOW_OK, "Partial frame input");
    }
    for (unsigned i = 0; i < 100 && delivered.load() != 8; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(delivered.load() == 8, "Input probe completion");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    adapter.reset();
    auto snapshot = metrics->snapshot();
    check(snapshot.stages[0][0].count == (audio ? 0 : 1),
          "Input counts partial frames and excludes audio");
    check(snapshot.stages[0][1].count == 0, "No fabricated AU");
    gst_object_unref(before);
    gst_object_unref(after);
    gst_object_unref(input);
    gst_object_unref(pipeline);
}

void buffer_list_test() {
    auto metrics = std::make_shared<Telemetry>("buffer-list");
    metrics->begin_generation(1);
    auto *pipe = gst_parse_launch("appsrc name=input format=time ! identity name=mark ! "
                                  "appsink name=output sync=false buffer-list=true",
                                  nullptr);
    auto *input = gst_bin_get_by_name(GST_BIN(pipe), "input"),
         *output = gst_bin_get_by_name(GST_BIN(pipe), "output"), *mark = pad(pipe, "mark", "src");
    auto adapter = std::make_unique<PipelineTelemetry>(metrics);
    adapter->attach_stage(mark, TimingStage::Draw);
    gst_element_set_state(pipe, GST_STATE_PLAYING);
    auto *list = gst_buffer_list_new();
    std::vector<GstBuffer *> retained;
    for (unsigned i = 0; i < 3; i++) {
        auto *b = gst_buffer_new();
        TraceToken token{metrics, metrics->frame(Telemetry::monotonic_ns())};
        check(set_frame_trace(b, token), "List input metadata");
        retained.push_back(gst_buffer_ref(b)); // Force copy-on-write of buffer headers.
        gst_buffer_list_add(list, b);
    }
    check(gst_app_src_push_buffer_list(GST_APP_SRC(input), list) == GST_FLOW_OK, "Push list");
    auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(output), 5 * GST_SECOND);
    check(sample, "List output");
    auto *received = gst_sample_get_buffer_list(sample);
    check(received && gst_buffer_list_length(received) == 3, "List preserved");
    for (unsigned i = 0; i < 3; i++) {
        auto token = frame_trace(gst_buffer_list_get(received, i));
        check(token.frame.ns[unsigned(TimingStage::Draw)] > 0, "Every list buffer marked");
        check(frame_trace(retained[i]).frame.ns[unsigned(TimingStage::Draw)] == 0,
              "Shared list input not mutated");
        gst_buffer_unref(retained[i]);
    }
    check(metrics->snapshot().stages[0][unsigned(TimingStage::Draw)].count == 3,
          "List stage count");
    gst_sample_unref(sample);
    gst_element_set_state(pipe, GST_STATE_NULL);
    adapter.reset();
    for (auto *object : {input, output})
        gst_object_unref(object);
    gst_object_unref(mark);
    gst_object_unref(pipe);
}

void submission_bounds() {
    int64_t now = 1000000000LL;
    auto metrics = std::make_shared<Telemetry>("bounded", TelemetryOptions{}, [&] { return now; });
    metrics->begin_generation(1);
    auto adapter = std::make_unique<PipelineTelemetry>(metrics);
    for (unsigned i = 0; i < 4100; i++) {
        now++;
        auto *buffer = gst_buffer_new();
        GST_BUFFER_PTS(buffer) = i;
        TraceToken token{metrics, metrics->frame(now)};
        set_frame_trace(buffer, token);
        adapter->submit(buffer);
        gst_buffer_unref(buffer);
    }
    check(metrics->snapshot().capacity_evicted >= 4, "Bounded correspondence table");
    adapter.reset();
    check(metrics->snapshot().submitted_without_output == 4100,
          "All retired submissions accounted");
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    try {
        std::atomic<int64_t> at{1000000000LL};
        auto metrics =
            std::make_shared<Telemetry>("synthetic", TelemetryOptions{}, [&] { return at.load(); });
        metrics->begin_generation(1);
        GError *error{};
        auto *pipe = gst_parse_launch(
            "appsrc name=input is-live=true format=time "
            "caps=\"application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=96\" "
            "! identity name=before ! identity name=jitter ! identity name=parser ! identity "
            "name=decoder ! appsink name=output sync=false",
            &error);
        check(pipe && !error, "Pipeline parse");
        auto *before = pad(pipe, "before", "sink"), *after = pad(pipe, "jitter", "src"),
             *au = pad(pipe, "parser", "src"), *delay = pad(pipe, "jitter", "sink"),
             *done = pad(pipe, "decoder", "src");
        auto add_delay = [](GstPad *, GstPadProbeInfo *, gpointer data) {
            *static_cast<std::atomic<int64_t> *>(data) += 20000000;
            return GST_PAD_PROBE_OK;
        };
        gst_pad_add_probe(delay, GST_PAD_PROBE_TYPE_BUFFER, add_delay, &at, nullptr);
        gst_pad_add_probe(done, GST_PAD_PROBE_TYPE_BUFFER, add_delay, &at, nullptr);
        auto adapter = std::make_unique<PipelineTelemetry>(metrics);
        adapter->attach_rtp(before, after);
        adapter->attach_access_units(au);
        auto *decoder = gst_bin_get_by_name(GST_BIN(pipe), "decoder");
        adapter->attach_decoder(decoder);
        gst_object_unref(decoder);
        auto *input = gst_bin_get_by_name(GST_BIN(pipe), "input"),
             *output = gst_bin_get_by_name(GST_BIN(pipe), "output");
        gst_element_set_state(pipe, GST_STATE_PLAYING);
        TraceToken retained;
        for (unsigned i = 0; i < 12; i++) {
            auto *b = gst_rtp_buffer_new_allocate(4, 0, 0);
            GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
            gst_rtp_buffer_map(b, GST_MAP_WRITE, &rtp);
            gst_rtp_buffer_set_ssrc(&rtp, 42);
            gst_rtp_buffer_set_timestamp(&rtp, 0xfffff000u + i * 3000u);
            gst_rtp_buffer_set_seq(&rtp, uint16_t(65530u + i));
            gst_rtp_buffer_unmap(&rtp);
            GST_BUFFER_PTS(b) = i * GST_SECOND / 30;
            check(gst_app_src_push_buffer(GST_APP_SRC(input), b) == GST_FLOW_OK, "Push");
            auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(output), GST_SECOND * 5);
            check(sample, "Output");
            auto *buffer = gst_sample_get_buffer(sample);
            retained = frame_trace(buffer);
            check(bool(retained.telemetry), "Output trace");
            check(retained.frame.ns[3] - retained.frame.ns[0] == 40000000,
                  "Controlled input/decode latency");
            auto *copy = gst_buffer_copy(buffer);
            check(frame_trace(copy).frame.sequence == retained.frame.sequence, "Metadata copy");
            gst_buffer_unref(copy);
            gst_sample_unref(sample);
        }
        auto s = metrics->snapshot();
        check(s.latency[0][0].samples == 12 && s.latency[0][1].mean_ms == 20, "Aggregation");
        check(!s.unmatched_rtp && !s.unmatched_decode, "Matching including RTP wrap");
        gst_element_set_state(pipe, GST_STATE_NULL);
        adapter.reset();
        gst_object_unref(input);
        gst_object_unref(output);
        for (auto *p : {before, after, au, delay, done})
            gst_object_unref(p);
        gst_object_unref(pipe);
        check(retained.mark(TimingStage::Draw, at.load() + 1000000), "Token outlives pipeline");
        reorder_test(at);
        input_only_test(false);
        input_only_test(true);
        submission_bounds();
        buffer_list_test();
        std::cout
            << "GStreamer RTP, delay attribution, decode, wrap and metadata ownership passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
