#include "amdva/gst_telemetry.hpp"
#include <chrono>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace amdva;
void require(bool v, const char *why) {
    if (!v)
        throw std::runtime_error(why);
}
struct Packets {
    std::vector<GstBuffer *> buffers;
    GstCaps *caps{};
    ~Packets() {
        for (auto *b : buffers)
            gst_buffer_unref(b);
        if (caps)
            gst_caps_unref(caps);
    }
};
GstElement *parse(const std::string &s) {
    GError *e{};
    auto *p = gst_parse_launch(s.c_str(), &e);
    if (e) {
        std::string why = e->message;
        g_clear_error(&e);
        if (p)
            gst_object_unref(p);
        throw std::runtime_error(why);
    }
    require(p, "Pipeline unavailable");
    return p;
}
void error(GstElement *p) {
    auto *bus = gst_element_get_bus(p);
    auto *m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (m) {
        GError *e{};
        gchar *debug{};
        gst_message_parse_error(m, &e, &debug);
        std::string why = e ? e->message : "Pipeline error";
        g_clear_error(&e);
        g_free(debug);
        gst_message_unref(m);
        throw std::runtime_error(why);
    }
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: amdva-validate-timing-rtp INPUT.mkv h264|h265 [normal|reorder|loss]\n";
        return 1;
    }
    try {
        const std::string codec = argv[2], mode = argc == 4 ? argv[3] : "normal";
        require(codec == "h264" || codec == "h265", "Invalid codec");
        require(mode == "normal" || mode == "reorder" || mode == "loss", "Invalid mode");
        Packets packets;
        auto *producer =
            parse("filesrc name=file ! matroskademux ! " + codec + "parse ! rtp" + codec +
                  "pay mtu=400 timestamp-offset=4294960000 seqnum-offset=65520 ssrc=42 ! appsink "
                  "name=packets sync=false");
        auto *file = gst_bin_get_by_name(GST_BIN(producer), "file");
        g_object_set(file, "location", argv[1], nullptr);
        gst_object_unref(file);
        auto *sink = gst_bin_get_by_name(GST_BIN(producer), "packets");
        gst_element_set_state(producer, GST_STATE_PLAYING);
        while (auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND)) {
            packets.buffers.push_back(gst_buffer_ref(gst_sample_get_buffer(sample)));
            if (!packets.caps)
                packets.caps = gst_caps_ref(gst_sample_get_caps(sample));
            gst_sample_unref(sample);
        }
        gst_element_set_state(producer, GST_STATE_NULL);
        error(producer);
        gst_object_unref(sink);
        gst_object_unref(producer);
        require(!packets.buffers.empty(), "No RTP fixture packets");
        auto *pipeline = parse("appsrc name=input is-live=true format=time ! rtpjitterbuffer "
                               "name=jitter latency=50 do-lost=true ! rtp" +
                               codec + "depay ! " + codec + "parse name=parser ! va" + codec +
                               "dec name=decoder ! video/x-raw(memory:VAMemory),format=NV12 ! "
                               "appsink name=output sync=false");
        auto *input = gst_bin_get_by_name(GST_BIN(pipeline), "input"),
             *output = gst_bin_get_by_name(GST_BIN(pipeline), "output"),
             *jitter = gst_bin_get_by_name(GST_BIN(pipeline), "jitter"),
             *parser = gst_bin_get_by_name(GST_BIN(pipeline), "parser"),
             *decoder = gst_bin_get_by_name(GST_BIN(pipeline), "decoder");
        gst_app_src_set_caps(GST_APP_SRC(input), packets.caps);
        auto collector = std::make_shared<Telemetry>(codec + "-" + mode);
        collector->begin_generation(1);
        auto adapter = std::make_unique<PipelineTelemetry>(collector);
        auto *before = gst_element_get_static_pad(jitter, "sink"),
             *after = gst_element_get_static_pad(jitter, "src"),
             *au = gst_element_get_static_pad(parser, "src");
        adapter->attach_rtp(before, after);
        adapter->attach_access_units(au);
        adapter->attach_decoder(decoder);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        unsigned mutation_count = 0;
        GstFlowReturn feed_flow = GST_FLOW_OK;
        std::thread feed([&] {
            const auto start = std::chrono::steady_clock::now();
            GstClockTime origin = GST_CLOCK_TIME_NONE, last = 0;
            uint32_t previous = 0;
            unsigned group = 0, index = 0;
            for (size_t i = 0; i < packets.buffers.size(); i++) {
                GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
                gst_rtp_buffer_map(packets.buffers[i], GST_MAP_READ, &rtp);
                auto stamp = gst_rtp_buffer_get_timestamp(&rtp);
                gst_rtp_buffer_unmap(&rtp);
                if (i && stamp != previous) {
                    group++;
                    index = 0;
                }
                previous = stamp;
                auto at = GST_BUFFER_DTS(packets.buffers[i]);
                if (!GST_CLOCK_TIME_IS_VALID(at))
                    at = group * GST_SECOND / 30;
                if (origin == GST_CLOCK_TIME_NONE)
                    origin = at;
                at = at >= origin ? at - origin : last;
                last = std::max(last, at);
                std::this_thread::sleep_until(start + std::chrono::nanoseconds(last));
                auto send = [&](size_t n) {
                    auto *b = gst_buffer_copy(packets.buffers[n]);
                    GST_BUFFER_PTS(b) = GST_BUFFER_DTS(b) = last;
                    feed_flow = gst_app_src_push_buffer(GST_APP_SRC(input), b);
                };
                if (mode == "loss" && group == 15 && index == 0) {
                    mutation_count++;
                    index++;
                    continue;
                }
                if (mode == "reorder" && group % 10 == 5 && index == 0 &&
                    i + 1 < packets.buffers.size()) {
                    send(i + 1);
                    send(i);
                    send(i);
                    i++;
                    index += 2;
                    mutation_count++;
                    continue;
                }
                send(i);
                index++;
                if (feed_flow != GST_FLOW_OK)
                    break;
            }
            gst_app_src_end_of_stream(GST_APP_SRC(input));
        });
        unsigned frames = 0, reordered = 0;
        uint64_t previous = 0;
        bool matching = true;
        while (auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(output), 10 * GST_SECOND)) {
            auto token = frame_trace(gst_sample_get_buffer(sample));
            matching &= bool(token.telemetry) && token.frame.ns[0] > 0 &&
                        token.frame.ns[3] >= token.frame.ns[0];
            if (previous && token.frame.sequence < previous)
                reordered++;
            previous = token.frame.sequence;
            frames++;
            gst_sample_unref(sample);
        }
        feed.join();
        error(pipeline);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        adapter.reset();
        for (auto *pad : {before, after, au})
            gst_object_unref(pad);
        for (auto *e : {input, output, jitter, parser, decoder})
            gst_object_unref(e);
        gst_object_unref(pipeline);
        auto metrics = collector->snapshot();
        std::cout << "{\"frames\":" << frames << ",\"reordered_outputs\":" << reordered
                  << ",\"network_mutations\":" << mutation_count
                  << ",\"telemetry\":" << metrics.json() << "}\n";
        require(frames >= 30, "Insufficient decoded B-frame fixture outputs");
        require(feed_flow == GST_FLOW_OK, "Input push failed");
        if (mode != "loss") {
            require(matching, "A decoded frame lost its input correlation");
            require(!metrics.unmatched_decode && !metrics.unmatched_rtp,
                    "Unexpected unmatched correspondence");
            require(reordered > 0, "Fixture did not exercise B-frame output reordering");
        }
        if (mode != "normal")
            require(mutation_count > 0, "Network mutation was not exercised");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
