// Encoded-only local test server. Input files must have no B frames and a
// keyframe at their start; no video decoding/encoding runs in the server.
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
struct Video {
    std::vector<GstBuffer *> frames;
    GstCaps *caps{};
    bool hevc{};
    ~Video() {
        for (auto *b : frames)
            gst_buffer_unref(b);
        if (caps)
            gst_caps_unref(caps);
    }
};
struct Feed {
    Video *video;
    uint64_t frame{};
};
static void need(GstAppSrc *src, guint, gpointer data) {
    auto &f = *static_cast<Feed *>(data);
    auto *b = gst_buffer_copy(f.video->frames[f.frame % f.video->frames.size()]);
    GST_BUFFER_PTS(b) = GST_BUFFER_DTS(b) = gst_util_uint64_scale(f.frame++, GST_SECOND, 30);
    GST_BUFFER_DURATION(b) = GST_SECOND / 30;
    gst_app_src_push_buffer(src, b);
}
static void configure(GstRTSPMediaFactory *, GstRTSPMedia *media, gpointer data) {
    auto *v = static_cast<Video *>(data);
    auto *pipeline = gst_rtsp_media_get_element(media);
    auto *src = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), "video");
    gst_app_src_set_caps(GST_APP_SRC(src), v->caps);
    auto *feed = new Feed{v, 0};
    GstAppSrcCallbacks cb{};
    cb.need_data = need;
    gst_app_src_set_callbacks(GST_APP_SRC(src), &cb, feed,
                              [](gpointer p) { delete static_cast<Feed *>(p); });
    gst_object_unref(src);
    gst_object_unref(pipeline);
}
static std::unique_ptr<Video> load(const char *path, bool hevc) {
    auto v = std::make_unique<Video>();
    v->hevc = hevc;
    auto *pipeline = gst_pipeline_new(nullptr);
    auto *file = gst_element_factory_make("filesrc", nullptr);
    auto *parser = gst_element_factory_make(hevc ? "h265parse" : "h264parse", nullptr);
    auto *filter = gst_element_factory_make("capsfilter", nullptr);
    auto *sink = gst_element_factory_make("appsink", nullptr);
    if (!file || !parser || !filter || !sink)
        throw std::runtime_error("Test server plugins unavailable");
    g_object_set(file, "location", path, nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);
    auto *caps = gst_caps_from_string(hevc ? "video/x-h265,stream-format=byte-stream,alignment=au"
                                           : "video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(pipeline), file, parser, filter, sink, nullptr);
    if (!gst_element_link_many(file, parser, filter, sink, nullptr))
        throw std::runtime_error("Cannot load test stream");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    while (auto *s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND)) {
        v->frames.push_back(gst_buffer_ref(gst_sample_get_buffer(s)));
        if (!v->caps)
            v->caps = gst_caps_ref(gst_sample_get_caps(s));
        gst_sample_unref(s);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    if (v->frames.empty())
        throw std::runtime_error("Empty test stream");
    return v;
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc < 3 || argc > 5) {
        std::cerr << "Usage: amdva-nvr-rtsp-server H264_FILE H265_FILE [PORT] [--swap-codecs]\n";
        return 1;
    }
    try {
        auto avc = load(argv[1], false), hevc = load(argv[2], true);
        auto *server = gst_rtsp_server_new();
        g_object_set(server, "address", "127.0.0.1", "service", argc >= 4 ? argv[3] : "8554",
                     nullptr);
        auto *mounts = gst_rtsp_server_get_mount_points(server);
        for (unsigned i = 0; i < 32; ++i) {
            auto *v = (i + (argc == 5 && std::string(argv[4]) == "--swap-codecs" ? 1 : 0)) % 2
                          ? hevc.get()
                          : avc.get();
            auto *factory = gst_rtsp_media_factory_new();
            gst_rtsp_media_factory_set_shared(factory, TRUE);
            const auto launch =
                std::string("( appsrc name=video is-live=true format=time block=true "
                            "max-bytes=1048576 ! identity sync=true ! ") +
                (v->hevc ? "rtph265pay" : "rtph264pay") + " name=pay0 pt=96 config-interval=-1 )";
            gst_rtsp_media_factory_set_launch(factory, launch.c_str());
            g_signal_connect(factory, "media-configure", G_CALLBACK(configure), v);
            gst_rtsp_mount_points_add_factory(mounts, ("/cam" + std::to_string(i)).c_str(),
                                              factory);
        }
        gst_object_unref(mounts);
        if (!gst_rtsp_server_attach(server, nullptr))
            throw std::runtime_error("Cannot bind RTSP server");
        std::cout << "RTSP fixtures ready on localhost\n" << std::flush;
        auto *loop = g_main_loop_new(nullptr, FALSE);
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
        gst_object_unref(server);
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
