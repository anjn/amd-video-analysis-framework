#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <glib-unix.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace {
using Sample = std::shared_ptr<GstSample>;
using Caps = std::shared_ptr<GstCaps>;
namespace fs = std::filesystem;
struct Options {
    std::string address{"127.0.0.1"}, prefix{"/cam"};
    unsigned port{8554};
    double fallback_fps{30};
    std::vector<fs::path> paths;
};
void usage() {
    std::cout << "Usage: amdva-rtsp-files [OPTIONS] FILE...\n"
                 "       amdva-rtsp-files [OPTIONS] --list FILE_LIST\n\n"
                 "Each input is looped on a separate /cam0, /cam1, ... URL.\n"
                 "H.264/H.265 video in MP4, MKV or raw elementary files; video only.\n"
                 "Encoded passthrough: no decoding, encoding, Qt or AMD GPU required.\n\n"
                 "  --list PATH           UTF-8 text, one input path per line; repeatable\n"
                 "                        Paths are relative to the list file; # comments allowed\n"
                 "  --address IP          Bind address (default: 127.0.0.1)\n"
                 "  --port N              RTSP port, 1..65535 (default: 8554)\n"
                 "  --prefix /NAME        URL prefix (default: /cam)\n"
                 "  --fallback-fps N      FPS only when input timing is unavailable (default: 30)\n"
                 "  --help                Show this help\n"
                 "  --                    End options, for paths beginning with '-'\n";
}
void add_list(Options &o, const fs::path &path) {
    std::ifstream list(path);
    if (!list)
        throw std::runtime_error("Cannot open file list: " + path.string());
    std::string line;
    bool first = true;
    while (std::getline(list, line)) {
        if (first && line.compare(0, 3, "\xef\xbb\xbf") == 0)
            line.erase(0, 3);
        first = false;
        auto begin = line.find_first_not_of(" \t\r"), end = line.find_last_not_of(" \t\r");
        if (begin == std::string::npos || line[begin] == '#')
            continue;
        o.paths.push_back(path.parent_path() / line.substr(begin, end - begin + 1));
    }
    if (list.bad())
        throw std::runtime_error("Cannot read file list: " + path.string());
}
Options options(int argc, char **argv) {
    Options o;
    bool positional = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() {
            if (++i == argc)
                throw std::runtime_error("Missing value for " + arg);
            return std::string(argv[i]);
        };
        if (!positional && arg == "--") {
            positional = true;
            continue;
        }
        if (!positional && arg == "--list")
            add_list(o, value());
        else if (!positional && arg == "--address")
            o.address = value();
        else if (!positional && arg == "--prefix")
            o.prefix = value();
        else if (!positional && arg == "--port") {
            auto text = value();
            unsigned port{};
            auto result = std::from_chars(text.data(), text.data() + text.size(), port);
            if (result.ec != std::errc() || result.ptr != text.data() + text.size() || port < 1 ||
                port > 65535)
                throw std::runtime_error("Invalid RTSP port");
            o.port = port;
        } else if (!positional && arg == "--fallback-fps") {
            auto text = value();
            size_t used{};
            o.fallback_fps = std::stod(text, &used);
            if (used != text.size() || !(o.fallback_fps > 0 && o.fallback_fps <= 240))
                throw std::runtime_error("Invalid fallback FPS (0 < FPS <= 240 required)");
        } else if (!positional && !arg.empty() && arg[0] == '-')
            throw std::runtime_error("Unknown option: " + arg);
        else
            o.paths.emplace_back(arg);
    }
    if (o.paths.empty())
        throw std::runtime_error("No input video files supplied");
    if (o.address.empty())
        throw std::runtime_error("Empty bind address");
    if (o.prefix.empty() || o.prefix[0] != '/' ||
        o.prefix.find_first_not_of(
            "/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::string::npos)
        throw std::runtime_error("Invalid URL prefix");
    for (auto &path : o.paths) {
        if (!fs::is_regular_file(path))
            throw std::runtime_error("Not a regular input file: " + path.string());
        path = fs::canonical(path);
    }
    return o;
}
// A separate, bounded demux/parser pipeline. It never autoplugs a decoder.
class Reader {
    GstElement *pipeline_{}, *sink_{};
    std::atomic<bool> linked_{false};
    std::mutex mutex_;
    std::string error_;
    void fail(const std::string &message) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = message;
    }
    static void pad_added(GstElement *, GstPad *pad, gpointer data) {
        auto &r = *static_cast<Reader *>(data);
        try {
            auto *caps = gst_pad_get_current_caps(pad);
            if (!caps)
                caps = gst_pad_query_caps(pad, nullptr);
            const std::string type = caps && !gst_caps_is_empty(caps) && !gst_caps_is_any(caps)
                                         ? gst_structure_get_name(gst_caps_get_structure(caps, 0))
                                         : "";
            if (caps)
                gst_caps_unref(caps);
            bool expected = false;
            const bool video = (type == "video/x-h264" || type == "video/x-h265") &&
                               r.linked_.compare_exchange_strong(expected, true);
            if (video) {
                const bool hevc = type == "video/x-h265";
                GError *error{};
                auto *branch = gst_parse_bin_from_description(
                    hevc
                        ? "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! h265parse "
                          "config-interval=-1 ! capsfilter "
                          "caps=\"video/x-h265,stream-format=byte-stream,alignment=au\""
                        : "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! h264parse "
                          "config-interval=-1 ! "
                          "capsfilter caps=\"video/x-h264,stream-format=byte-stream,alignment=au\"",
                    TRUE, &error);
                if (error || !branch) {
                    std::string message = error ? error->message : "Parser branch unavailable";
                    g_clear_error(&error);
                    if (branch)
                        gst_object_unref(branch);
                    throw std::runtime_error(message);
                }
                gst_bin_add(GST_BIN(r.pipeline_), branch);
                auto *input = gst_element_get_static_pad(branch, "sink");
                bool ok = gst_pad_link(pad, input) == GST_PAD_LINK_OK;
                gst_object_unref(input);
                if (!ok || !gst_element_link(branch, r.sink_) ||
                    !gst_element_sync_state_with_parent(branch))
                    throw std::runtime_error("Cannot link video parser");
            } else {
                // Drain audio, subtitles and additional tracks without decoding them.
                auto *discard = gst_element_factory_make("fakesink", nullptr);
                if (!discard)
                    throw std::runtime_error("fakesink unavailable");
                g_object_set(discard, "sync", FALSE, "async", FALSE, nullptr);
                gst_bin_add(GST_BIN(r.pipeline_), discard);
                auto *input = gst_element_get_static_pad(discard, "sink");
                bool ok = gst_pad_link(pad, input) == GST_PAD_LINK_OK;
                gst_object_unref(input);
                if (!ok || !gst_element_sync_state_with_parent(discard))
                    throw std::runtime_error("Cannot discard unused track");
            }
        } catch (const std::exception &e) {
            r.fail(e.what());
        }
    }

  public:
    explicit Reader(const fs::path &path) {
        pipeline_ = gst_pipeline_new(nullptr);
        auto *source = gst_element_factory_make("filesrc", nullptr),
             *parse = gst_element_factory_make("parsebin", nullptr);
        sink_ = gst_element_factory_make("appsink", nullptr);
        if (!source || !parse || !sink_) {
            for (auto *p : {source, parse, sink_})
                if (p)
                    gst_object_unref(p);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            throw std::runtime_error("File server plugins unavailable");
        }
        gst_bin_add_many(GST_BIN(pipeline_), source, parse, sink_, nullptr);
        g_object_set(source, "location", path.c_str(), nullptr);
        g_object_set(sink_, "sync", FALSE, "async", FALSE, "max-buffers", 2u, "drop", FALSE,
                     "wait-on-eos", FALSE, nullptr);
        g_signal_connect(parse, "pad-added", G_CALLBACK(pad_added), this);
        g_signal_connect(parse, "drained", G_CALLBACK(+[](GstElement *, gpointer p) {
                             auto &r = *static_cast<Reader *>(p);
                             if (!r.linked_)
                                 r.fail("No H.264/H.265 video track");
                         }),
                         this);
        if (!gst_element_link(source, parse) ||
            gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            throw std::runtime_error("Cannot start file parser: " + path.string());
        }
    }
    ~Reader() {
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
        }
    }
    Reader(const Reader &) = delete;
    Sample next() {
        const auto deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        do {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!error_.empty())
                    throw std::runtime_error(error_);
            }
            auto *bus = gst_element_get_bus(pipeline_);
            auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
            gst_object_unref(bus);
            if (message) {
                GError *error{};
                gst_message_parse_error(message, &error, nullptr);
                std::string text = error ? error->message : "Input parser failed";
                g_clear_error(&error);
                gst_message_unref(message);
                throw std::runtime_error(text);
            }
            auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 100 * GST_MSECOND);
            if (sample)
                return {sample, [](auto *p) { gst_sample_unref(p); }};
            if (gst_app_sink_is_eos(GST_APP_SINK(sink_)))
                return {};
        } while (g_get_monotonic_time() < deadline);
        throw std::runtime_error("Timed out waiting for encoded video");
    }
};
struct File {
    fs::path path;
    Caps caps;
    bool hevc{};
    unsigned width{}, height{};
    double fps{};
};
std::shared_ptr<File> inspect(const fs::path &path) {
    Reader reader(path);
    auto first = reader.next();
    if (!first)
        throw std::runtime_error("Empty video: " + path.string());
    auto result = std::make_shared<File>();
    result->path = path;
    result->caps = {gst_caps_ref(gst_sample_get_caps(first.get())),
                    [](auto *p) { gst_caps_unref(p); }};
    const auto *s = gst_caps_get_structure(result->caps.get(), 0);
    result->hevc = g_str_equal(gst_structure_get_name(s), "video/x-h265");
    int width = 0, height = 0, n = 0, d = 1;
    gst_structure_get_int(s, "width", &width);
    gst_structure_get_int(s, "height", &height);
    gst_structure_get_fraction(s, "framerate", &n, &d);
    result->width = unsigned(std::max(0, width));
    result->height = unsigned(std::max(0, height));
    result->fps = d > 0 ? double(n) / d : 0;
    return result;
}
struct Feed {
    std::shared_ptr<const File> file;
    std::unique_ptr<Reader> reader;
    GstClockTime offset{}, origin{GST_CLOCK_TIME_NONE}, min_pts{GST_CLOCK_TIME_NONE},
        min_dts{GST_CLOCK_TIME_NONE}, pts_end{}, dts_end{}, generated{},
        last_dts{GST_CLOCK_TIME_NONE};
    GstClockTime step;
    uint64_t count{}, loops{};
    bool failed{}, synthetic_dts{};
    Feed(std::shared_ptr<const File> f, double fallback)
        : file(std::move(f)),
          step(GstClockTime(GST_SECOND / (file->fps > 0 ? file->fps : fallback))) {}
    GstClockTime span() const {
        GstClockTime result = step;
        if (GST_CLOCK_TIME_IS_VALID(min_pts))
            result = std::max(result, pts_end - min_pts);
        if (GST_CLOCK_TIME_IS_VALID(min_dts))
            result = std::max(result, dts_end - min_dts);
        return result;
    }
    GstBuffer *next() {
        if (!reader)
            reader = std::make_unique<Reader>(file->path);
        auto sample = reader->next();
        if (!sample) {
            if (!count)
                throw std::runtime_error("Empty video during replay");
            offset += span();
            ++loops;
            origin = min_pts = min_dts = GST_CLOCK_TIME_NONE;
            pts_end = dts_end = generated = 0;
            reader = std::make_unique<Reader>(file->path);
            sample = reader->next();
            if (!sample)
                throw std::runtime_error("Video became empty during replay");
        }
        auto *source = gst_sample_get_buffer(sample.get());
        const auto duration =
            GST_BUFFER_DURATION_IS_VALID(source) && GST_BUFFER_DURATION(source) > 0
                ? GST_BUFFER_DURATION(source)
                : step;
        auto pts = GST_BUFFER_PTS(source), dts = GST_BUFFER_DTS(source);
        if (!GST_CLOCK_TIME_IS_VALID(pts))
            pts = GST_CLOCK_TIME_IS_VALID(dts) ? dts : generated;
        if (!GST_CLOCK_TIME_IS_VALID(origin)) {
            synthetic_dts = !GST_CLOCK_TIME_IS_VALID(dts);
            generated = pts;
        }
        // Matroska commonly omits DTS even with B frames. Preserve its reordered
        // PTS and synthesize a separate decode clock from AU durations.
        if (synthetic_dts || !GST_CLOCK_TIME_IS_VALID(dts))
            dts = generated;
        if (!GST_CLOCK_TIME_IS_VALID(origin))
            origin = std::min(pts, dts);
        if (pts < origin || dts < origin)
            throw std::runtime_error("Input timestamp precedes its decode origin");
        min_pts = std::min(min_pts, pts);
        min_dts = std::min(min_dts, dts);
        pts_end = std::max(pts_end, pts + duration);
        dts_end = std::max(dts_end, dts + duration);
        generated = dts + duration;
        auto *buffer = gst_buffer_copy(source); // Encoded payload memory remains shared.
        GST_BUFFER_PTS(buffer) = offset + pts - origin;
        GST_BUFFER_DTS(buffer) = offset + dts - origin;
        GST_BUFFER_DURATION(buffer) = duration;
        if (GST_CLOCK_TIME_IS_VALID(last_dts) && GST_BUFFER_DTS(buffer) < last_dts) {
            gst_buffer_unref(buffer);
            throw std::runtime_error("Non-monotonic input decode timestamps");
        }
        last_dts = GST_BUFFER_DTS(buffer);
        if (count++)
            GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DISCONT);
        return buffer;
    }
};
struct FactoryData {
    std::shared_ptr<const File> file;
    double fps;
};
void configure(GstRTSPMediaFactory *, GstRTSPMedia *media, gpointer data) {
    auto &factory = *static_cast<FactoryData *>(data);
    auto *pipeline = gst_rtsp_media_get_element(media);
    auto *src = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), "video");
    if (src) {
        gst_app_src_set_caps(GST_APP_SRC(src), factory.file->caps.get());
        auto *feed = new Feed(factory.file, factory.fps);
        GstAppSrcCallbacks callbacks{};
        callbacks.need_data = +[](GstAppSrc *src, guint, gpointer p) {
            auto &feed = *static_cast<Feed *>(p);
            try {
                if (!feed.failed)
                    gst_app_src_push_buffer(src, feed.next());
            } catch (const std::exception &e) {
                feed.failed = true;
                std::cerr << feed.file->path << ": " << e.what() << '\n';
                gst_app_src_end_of_stream(src);
            }
        };
        gst_app_src_set_callbacks(GST_APP_SRC(src), &callbacks, feed,
                                  [](gpointer p) { delete static_cast<Feed *>(p); });
        gst_object_unref(src);
    }
    gst_object_unref(pipeline);
}
} // namespace
int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--")
            break;
        if (std::string(argv[i]) == "--help") {
            usage();
            return 0;
        }
    }
    try {
        const auto opt = options(argc, argv);
        gst_init(nullptr, nullptr);
        std::vector<std::shared_ptr<File>> files;
        for (const auto &path : opt.paths) {
            try {
                files.push_back(inspect(path));
            } catch (const std::exception &e) {
                throw std::runtime_error(path.string() + ": " + e.what());
            }
        }
        auto *server = gst_rtsp_server_new();
        g_object_set(server, "address", opt.address.c_str(), "service",
                     std::to_string(opt.port).c_str(), nullptr);
        auto *mounts = gst_rtsp_server_get_mount_points(server);
        for (size_t i = 0; i < files.size(); ++i) {
            auto *factory = gst_rtsp_media_factory_new();
            gst_rtsp_media_factory_set_shared(factory, TRUE);
            const auto launch =
                std::string("( appsrc name=video is-live=true format=time block=true "
                            "max-bytes=1048576 max-buffers=8 ! identity sync=true ! ") +
                (files[i]->hevc ? "rtph265pay" : "rtph264pay") +
                " name=pay0 pt=96 config-interval=-1 )";
            gst_rtsp_media_factory_set_launch(factory, launch.c_str());
            auto *data = new FactoryData{files[i], opt.fallback_fps};
            g_signal_connect_data(
                factory, "media-configure", G_CALLBACK(configure), data,
                [](gpointer p, GClosure *) { delete static_cast<FactoryData *>(p); },
                GConnectFlags(0));
            gst_rtsp_mount_points_add_factory(mounts, (opt.prefix + std::to_string(i)).c_str(),
                                              factory);
        }
        gst_object_unref(mounts);
        const auto attached = gst_rtsp_server_attach(server, nullptr);
        if (!attached) {
            gst_object_unref(server);
            throw std::runtime_error("Cannot bind RTSP server");
        }
        for (size_t i = 0; i < files.size(); ++i) {
            auto host = opt.address == "0.0.0.0" ? "127.0.0.1"
                        : opt.address == "::"    ? "::1"
                                                 : opt.address;
            if (host.find(':') != std::string::npos)
                host = "[" + host + "]";
            std::cout << "rtsp://" << host << ':' << opt.port << opt.prefix << i << '\n';
            std::cerr << opt.prefix << i << " <- " << files[i]->path << " ["
                      << (files[i]->hevc ? "H.265" : "H.264") << ", " << files[i]->width << 'x'
                      << files[i]->height << ", "
                      << (files[i]->fps > 0 ? files[i]->fps : opt.fallback_fps) << " fps"
                      << (files[i]->fps > 0 ? "" : " fallback") << "]\n";
        }
        std::cout << std::flush;
        auto *loop = g_main_loop_new(nullptr, FALSE);
        const auto interrupt = g_unix_signal_add(
            SIGINT,
            [](gpointer p) -> gboolean {
                g_main_loop_quit(static_cast<GMainLoop *>(p));
                return G_SOURCE_CONTINUE;
            },
            loop);
        const auto terminate = g_unix_signal_add(
            SIGTERM,
            [](gpointer p) -> gboolean {
                g_main_loop_quit(static_cast<GMainLoop *>(p));
                return G_SOURCE_CONTINUE;
            },
            loop);
        const auto cleanup = g_timeout_add_seconds(
            2,
            [](gpointer p) -> gboolean {
                auto *pool = gst_rtsp_server_get_session_pool(GST_RTSP_SERVER(p));
                gst_rtsp_session_pool_cleanup(pool);
                gst_object_unref(pool);
                return G_SOURCE_CONTINUE;
            },
            server);
        g_main_loop_run(loop);
        g_source_remove(interrupt);
        g_source_remove(terminate);
        g_source_remove(cleanup);
        g_source_remove(attached);
        auto *remaining = gst_rtsp_server_client_filter(
            server,
            [](GstRTSPServer *, GstRTSPClient *client, gpointer) -> GstRTSPFilterResult {
                gst_rtsp_client_close(client);
                return GST_RTSP_FILTER_REMOVE;
            },
            nullptr);
        g_list_free_full(remaining, g_object_unref);
        gst_object_unref(server);
        g_main_loop_unref(loop);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "amdva-rtsp-files: " << e.what() << '\n';
        return 1;
    }
}
