// Decode -> detect -> overlay -> hardware encode, written to a file or served over
// RTSP. Frames stay on the GPU from the decoder to the encoder (VAMemory, then a
// BGRA DMA-BUF overlay surface, then NV12 VAMemory); only the compressed stream
// reaches the CPU.
//
//   transcode IN.h264 MODEL.mxr MODEL_INFO.json --output out.mp4 [--codec h264|h265|av1]
//   transcode IN.h264 MODEL.mxr MODEL_INFO.json --rtsp 8554 [--mount /annotated]
//
// RTSP serving supports h264 and h265; av1 is for files (RTP AV1 clients are rare).
#include <amdva/rtsp_server.hpp>
#include <glib-unix.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace {
struct Options {
    std::string input, model, info, codec{"h264"}, output, mount{"/annotated"};
    unsigned port{};
};
// The RTSP side owns an appsrc that exists only while a client is connected.
struct Relay {
    std::mutex mutex;
    GstAppSrc *src{};
    GstCaps *caps{};
    ~Relay() {
        if (caps)
            gst_caps_unref(caps);
    }
};
std::string quoted(const std::string &value) { return "\"" + value + "\""; }
const char *payloader(const std::string &codec) {
    return codec == "h265" ? "rtph265pay" : codec == "av1" ? "rtpav1pay" : "rtph264pay";
}
const char *muxer(const std::string &output) {
    return output.size() > 4 && output.substr(output.size() - 4) == ".mkv" ? "matroskamux"
                                                                              : "mp4mux";
}
} // namespace

int main(int argc, char **argv) {
    Options options;
    std::string *positional[] = {&options.input, &options.model, &options.info};
    size_t index = 0;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 == argc)
                throw std::runtime_error("Missing value for " + arg);
            return argv[++i];
        };
        try {
            if (arg == "--output")
                options.output = next();
            else if (arg == "--rtsp")
                options.port = unsigned(std::stoul(next()));
            else if (arg == "--mount")
                options.mount = next();
            else if (arg == "--codec")
                options.codec = next();
            else if (index < 3)
                *positional[index++] = arg;
            else
                throw std::runtime_error("Unexpected argument " + arg);
        } catch (const std::exception &e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    }
    if (index != 3 || (options.output.empty() == (options.port == 0))) {
        std::cerr << "Usage: transcode IN.h264 MODEL.mxr MODEL_INFO.json "
                     "(--output FILE.mp4|FILE.mkv | --rtsp PORT [--mount /path]) "
                     "[--codec h264|h265|av1]\n";
        return 1;
    }
    gst_init(&argc, &argv);
    try {
        const std::string analysis =
            "filesrc location=" + quoted(options.input) +
            " ! h264parse ! video/x-h264,framerate=30/1 ! vah264dec ! "
            "video/x-raw(memory:VAMemory),format=NV12 ! amddetect model=" +
            quoted(options.model) + " model-info=" + quoted(options.info) +
            " ! amdwatermark ! amdencode codec=" + options.codec + " bitrate=6000";
        GError *error{};
        GstElement *pipeline{};
        Relay relay;
        std::unique_ptr<amdva::RtspServer> server;
        auto *loop = g_main_loop_new(nullptr, FALSE);
        if (!options.output.empty()) {
            pipeline = gst_parse_launch((analysis + " ! " + options.codec + "parse ! " +
                                         muxer(options.output) + " ! filesink location=" +
                                         quoted(options.output))
                                            .c_str(),
                                        &error);
        } else {
            pipeline = gst_parse_launch((analysis + " ! appsink name=encoded sync=true "
                                                    "emit-signals=false max-buffers=8 drop=false")
                                            .c_str(),
                                        &error);
            server = std::make_unique<amdva::RtspServer>(
                amdva::RtspServer::Options{"0.0.0.0", options.port});
            server->add_mount(
                options.mount,
                std::string("( appsrc name=video is-live=true format=time do-timestamp=true block=false ! ") +
                    // The parser repeats SPS/PPS (VPS) before every keyframe so a client can
                    // join mid-stream.
                    (options.codec == "av1" ? "av1parse" : options.codec + "parse config-interval=-1") +
                    " ! " + payloader(options.codec) + " name=pay0 pt=96 config-interval=-1 )",
                [&relay](GstRTSPMedia *, GstElement *rtsp_pipeline) {
                    auto *src = gst_bin_get_by_name_recurse_up(GST_BIN(rtsp_pipeline), "video");
                    std::lock_guard<std::mutex> lock(relay.mutex);
                    if (relay.caps)
                        gst_app_src_set_caps(GST_APP_SRC(src), relay.caps);
                    relay.src = GST_APP_SRC(src);
                });
            server->attach();
            std::cout << server->url(options.mount) << std::endl;
        }
        if (!pipeline || error)
            throw std::runtime_error(error ? error->message : "Cannot build pipeline");
        auto *bus = gst_element_get_bus(pipeline);
        gst_bus_add_watch(
            bus,
            [](GstBus *, GstMessage *message, gpointer data) -> gboolean {
                auto *main_loop = static_cast<GMainLoop *>(data);
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                    GError *e{};
                    gst_message_parse_error(message, &e, nullptr);
                    std::cerr << (e ? e->message : "pipeline error") << "\n";
                    g_clear_error(&e);
                    g_main_loop_quit(main_loop);
                } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
                    g_main_loop_quit(main_loop);
                }
                return TRUE;
            },
            loop);
        if (server) {
            auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "encoded");
            GstAppSinkCallbacks callbacks{};
            callbacks.new_sample = [](GstAppSink *appsink, gpointer data) -> GstFlowReturn {
                auto &r = *static_cast<Relay *>(data);
                auto *sample = gst_app_sink_pull_sample(appsink);
                if (!sample)
                    return GST_FLOW_EOS;
                std::lock_guard<std::mutex> lock(r.mutex);
                if (!r.caps)
                    r.caps = gst_caps_copy(gst_sample_get_caps(sample));
                if (r.src) {
                    // appsink is clocked (sync=true), so arrival time is the live timeline
                    // of the RTSP pipeline; do-timestamp stamps it there.
                    auto *buffer = gst_buffer_copy(gst_sample_get_buffer(sample));
                    GST_BUFFER_PTS(buffer) = GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
                    gst_app_src_push_buffer(r.src, buffer);
                }
                gst_sample_unref(sample);
                return GST_FLOW_OK;
            };
            gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, &relay, nullptr);
            gst_object_unref(sink);
        }
        // A file needs EOS to finalize its index; the message watch then ends the loop.
        struct Stop {
            GMainLoop *loop;
            GstElement *pipeline;
            bool finalize;
        } stop{loop, pipeline, !options.output.empty()};
        g_unix_signal_add(
            SIGINT,
            [](gpointer data) -> gboolean {
                auto &s = *static_cast<Stop *>(data);
                if (s.finalize)
                    gst_element_send_event(s.pipeline, gst_event_new_eos());
                else
                    g_main_loop_quit(s.loop);
                return G_SOURCE_REMOVE;
            },
            &stop);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        g_main_loop_run(loop);
        if (server)
            server->disconnect_clients();
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        g_main_loop_unref(loop);
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
