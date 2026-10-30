#include "amdva/rtsp_server.hpp"
#include "amdva/vui.hpp"
#include "renderer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <dlfcn.h>
#include <glib-unix.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/va/gstvautils.h>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
using namespace amdva;
using namespace amdva::dashboard;
namespace {
struct Options {
    std::string bind{"0.0.0.0"}, mount{"/dashboard"}, node{"/dev/dri/renderD128"}, font;
    // Display names; empty uses the detected CPU and GPU names.
    std::string cpu_name, gpu_name;
    unsigned port{8554}, fps{10}, bitrate{4000}, seconds{};
    double power{48};
};
Options parse(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::cout
                << "Usage: amdva-dashboard [--bind ADDRESS] [--port 8554] [--mount /dashboard]\n  "
                   "[--render-node /dev/dri/renderD128] [--fps 10] [--bitrate-kbps 4000]\n  "
                   "[--power-max-w 48] [--font FILE] [--duration-seconds N]\n  "
                   "[--cpu-name TEXT] [--gpu-name TEXT]\n";
            std::exit(0);
        }
        if (i + 1 == argc)
            throw std::runtime_error("Missing value for " + arg);
        const std::string value = argv[++i];
        auto number = [&] {
            size_t used{};
            const double n = std::stod(value, &used);
            if (used != value.size() || !std::isfinite(n) || n <= 0)
                throw std::runtime_error("Invalid value for " + arg);
            return n;
        };
        auto integer = [&](unsigned max) {
            double n = number();
            if (std::floor(n) != n || n > max)
                throw std::runtime_error("Out of range: " + arg);
            return unsigned(n);
        };
        if (arg == "--bind")
            o.bind = value;
        else if (arg == "--mount")
            o.mount = value;
        else if (arg == "--render-node")
            o.node = value;
        else if (arg == "--font")
            o.font = value;
        else if (arg == "--port")
            o.port = integer(65535);
        else if (arg == "--fps")
            o.fps = integer(60);
        else if (arg == "--bitrate-kbps")
            o.bitrate = integer(2048000);
        else if (arg == "--power-max-w")
            o.power = number();
        else if (arg == "--duration-seconds")
            o.seconds = integer(86400);
        else if (arg == "--cpu-name" || arg == "--gpu-name") {
            // The glyph atlas holds printable ASCII only.
            if (value.size() > 120 || std::any_of(value.begin(), value.end(), [](char c) {
                    return c < 32 || c > 126;
                }))
                throw std::runtime_error(arg + " must be printable ASCII, up to 120 characters");
            (arg == "--cpu-name" ? o.cpu_name : o.gpu_name) = value;
        }
        else
            throw std::runtime_error("Unknown option: " + arg);
    }
    if (o.bind.empty() || o.mount.size() < 2 || o.mount.front() != '/' ||
        o.mount.find_first_of(" \t\n?#") != std::string::npos)
        throw std::runtime_error("Invalid bind address or RTSP mount");
    return o;
}
struct App;
struct Media {
    App &app;
    GstElement *src{};
    std::atomic<bool> running{false}, want{true};
    std::thread worker;
    GWeakRef media_ref;
    explicit Media(App &a) : app(a) { g_weak_ref_init(&media_ref, nullptr); }
    ~Media() {
        stop();
        g_weak_ref_clear(&media_ref);
        if (src) {
            GstAppSrcCallbacks empty{};
            gst_app_src_set_callbacks(GST_APP_SRC(src), &empty, nullptr, nullptr);
            gst_object_unref(src);
        }
    }
    void stop() {
        running = false;
        if (worker.joinable())
            worker.join();
    }
    void start();
    void unprepare() {
        stop();
        if (auto *object = g_weak_ref_get(&media_ref)) {
            gst_rtsp_media_unprepare(GST_RTSP_MEDIA(object));
            g_object_unref(object);
        }
    }
};
struct App {
    Options options;
    ResourceMonitor monitor;
    Renderer renderer;
    std::mutex mutex, render_mutex;
    std::deque<ResourceSample> history;
    std::atomic<bool> running{true}, failed{false};
    std::thread sampler;
    GMainLoop *loop{};
    std::atomic<uint64_t> frames{0}, skipped{0}, accepted{0};
    std::mutex surfaces_mutex;
    std::set<VASurfaceID> submitted;
    std::mutex media_mutex;
    std::vector<std::weak_ptr<Media>> media;
    App(Options o)
        : options(o), monitor(ResourceMonitorConfig{o.node}),
          renderer(o.node, o.font, o.fps, o.power) {
        history.push_back(monitor.sample());
        sampler = std::thread([this] {
            auto next = std::chrono::steady_clock::now();
            while (running) {
                next += std::chrono::milliseconds(100);
                ResourceSample s;
                try {
                    s = monitor.sample();
                } catch (const std::exception &e) {
                    error(e.what());
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    history.push_back(s);
                    while (history.size() > 601 ||
                           s.monotonic_ns - history.front().monotonic_ns > 60000000000LL)
                        history.pop_front();
                }
                std::this_thread::sleep_until(next);
                if (std::chrono::steady_clock::now() > next + std::chrono::seconds(1))
                    next = std::chrono::steady_clock::now();
            }
        });
    }
    ~App() {
        running = false;
        if (sampler.joinable())
            sampler.join();
        std::vector<std::shared_ptr<Media>> active;
        {
            std::lock_guard<std::mutex> lock(media_mutex);
            for (auto &weak : media)
                if (auto m = weak.lock())
                    active.push_back(m);
        }
        for (auto &m : active)
            m->stop();
    }
    void error(const std::string &message) {
        std::cerr << "Dashboard error: " << message << "\n";
        failed = true;
        if (loop)
            g_main_loop_quit(loop);
    }
};
void Media::start() {
    if (running.exchange(true))
        return;
    worker = std::thread([this] {
        const auto started = std::chrono::steady_clock::now();
        const auto period = std::chrono::nanoseconds(1000000000 / app.options.fps);
        uint64_t last_tick = UINT64_MAX;
        try {
            while (running && app.running) {
                auto now = std::chrono::steady_clock::now();
                auto tick = uint64_t(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - started).count() /
                    period.count());
                if (tick == last_tick || !want) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                last_tick = tick;
                std::deque<ResourceSample> snapshot;
                {
                    std::lock_guard<std::mutex> lock(app.mutex);
                    snapshot = app.history;
                }
                GstBuffer *buffer{};
                {
                    std::lock_guard<std::mutex> lock(app.render_mutex);
                    buffer = app.renderer.frame(
                        snapshot,
                        app.options.cpu_name.empty() ? app.monitor.cpu_name()
                                                     : app.options.cpu_name,
                        app.options.gpu_name.empty() ? app.monitor.gpu_name()
                                                     : app.options.gpu_name);
                }
                if (!buffer) {
                    app.skipped++;
                    continue;
                }
                GST_BUFFER_PTS(buffer) = tick * period.count();
                GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
                GST_BUFFER_DURATION(buffer) = period.count();
                {
                    std::lock_guard<std::mutex> lock(app.surfaces_mutex);
                    app.submitted.insert(gst_va_buffer_get_surface(buffer));
                }
                const auto flow = gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
                if (flow != GST_FLOW_OK)
                    break;
                app.frames++;
            }
        } catch (const std::exception &e) {
            app.error(e.what());
        }
        running = false;
    });
}
void configure(GstRTSPMedia *media, App *data) {
    auto &app = *data;
    auto state = std::make_shared<Media>(app);
    g_weak_ref_set(&state->media_ref, G_OBJECT(media));
    auto *pipeline = gst_rtsp_media_get_element(media);
    state->src = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), "video");
    if (!state->src) {
        gst_object_unref(pipeline);
        app.error("Missing dashboard appsrc");
        return;
    }
    g_signal_connect(media, "handle-message",
                     G_CALLBACK(+[](GstRTSPMedia *, GstMessage *message, gpointer p) -> gboolean {
                         if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                             GError *error{};
                             gchar *debug{};
                             gst_message_parse_error(message, &error, &debug);
                             static_cast<App *>(p)->error(error ? error->message
                                                                : "RTSP pipeline failed");
                             g_clear_error(&error);
                             g_free(debug);
                         }
                         return FALSE;
                     }),
                     &app);
    auto *context = gst_context_new(GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR, TRUE);
    gst_context_set_va_display(context, app.renderer.display());
    gst_element_set_context(pipeline, context);
    gst_context_unref(context);
    auto *enc = gst_bin_get_by_name_recurse_up(GST_BIN(pipeline), "encoder");
    if (!enc) {
        gst_object_unref(pipeline);
        app.error("Hardware H264 encoder unavailable");
        return;
    }
    auto *pad = gst_element_get_static_pad(enc, "sink");
    gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
            auto &a = *static_cast<App *>(data);
            auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
            const auto surface = gst_va_buffer_get_surface(buffer);
            if (surface == VA_INVALID_SURFACE ||
                gst_va_buffer_peek_display(buffer) != a.renderer.display())
                a.error("Encoder input left shared VAMemory");
            else {
                std::lock_guard<std::mutex> lock(a.surfaces_mutex);
                if (!a.submitted.count(surface))
                    a.error("Encoder input surface was replaced");
                else
                    a.accepted++;
            }
            return GST_PAD_PROBE_OK;
        },
        &app, nullptr);
    gst_object_unref(pad);
    auto *encoded = gst_element_get_static_pad(enc, "src");
    gst_pad_add_probe(
        encoded, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
            auto *original = GST_PAD_PROBE_INFO_BUFFER(info);
            try {
                auto *updated = signal_bt709(original);
                GST_PAD_PROBE_INFO_DATA(info) = updated;
                gst_buffer_unref(original);
            } catch (const std::exception &e) {
                static_cast<App *>(data)->error(e.what());
                return GST_PAD_PROBE_DROP;
            }
            return GST_PAD_PROBE_OK;
        },
        &app, nullptr);
    gst_object_unref(encoded);
    gst_object_unref(enc);
    auto *caps = gst_caps_from_string(
        ("video/x-raw(memory:VAMemory),format=NV12,width=1920,height=1080,framerate=" +
         std::to_string(app.options.fps) + "/1,colorimetry=bt709,interlace-mode=progressive")
            .c_str());
    gst_app_src_set_caps(GST_APP_SRC(state->src), caps);
    gst_caps_unref(caps);
    g_object_set(state->src, "is-live", TRUE, "format", GST_FORMAT_TIME, "max-buffers", guint64(2),
                 "max-bytes", guint64(0), "block", FALSE, nullptr);
    GstAppSrcCallbacks callbacks{};
    callbacks.need_data = [](GstAppSrc *, guint, gpointer p) {
        static_cast<Media *>(p)->want = true;
    };
    callbacks.enough_data = [](GstAppSrc *, gpointer p) { static_cast<Media *>(p)->want = false; };
    gst_app_src_set_callbacks(GST_APP_SRC(state->src), &callbacks, state.get(), nullptr);
    auto *holder = new std::shared_ptr<Media>(state);
    g_object_set_data_full(G_OBJECT(media), "dashboard-state", holder,
                           [](gpointer p) { delete static_cast<std::shared_ptr<Media> *>(p); });
    g_signal_connect(media, "unprepared", G_CALLBACK(+[](GstRTSPMedia *m, gpointer) {
                         auto *h = static_cast<std::shared_ptr<Media> *>(
                             g_object_get_data(G_OBJECT(m), "dashboard-state"));
                         if (h)
                             (*h)->stop();
                     }),
                     nullptr);
    {
        std::lock_guard<std::mutex> lock(app.media_mutex);
        app.media.erase(
            std::remove_if(app.media.begin(), app.media.end(), [](auto &w) { return w.expired(); }),
            app.media.end());
        app.media.push_back(state);
    }
    gst_object_unref(pipeline);
    state->start();
}
} // namespace
int main(int argc, char **argv) {
    try {
        const auto options = parse(argc, argv);
        gst_init(nullptr, nullptr);
        App app(options);
        app.loop = g_main_loop_new(nullptr, FALSE);
        RtspServer server({options.bind, options.port});
        const std::string launch =
            "( appsrc name=video ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! "
            "vah264enc name=encoder bitrate=" +
            std::to_string(options.bitrate) +
            " b-frames=0 key-int-max=" + std::to_string(options.fps) +
            " ! h264parse ! rtph264pay name=pay0 pt=96 config-interval=-1 )";
        server.add_mount(options.mount, launch, [&app](GstRTSPMedia *media, GstElement *) {
            configure(media, &app);
        });
        try {
            server.attach();
        } catch (...) {
            g_main_loop_unref(app.loop);
            app.loop = nullptr;
            throw;
        }
        auto quit = [](gpointer p) -> gboolean {
            g_main_loop_quit(static_cast<App *>(p)->loop);
            return G_SOURCE_CONTINUE;
        };
        auto sigint = g_unix_signal_add(SIGINT, quit, &app),
             sigterm = g_unix_signal_add(SIGTERM, quit, &app);
        guint timer = options.seconds ? g_timeout_add_seconds(options.seconds, quit, &app) : 0;
        if (auto gl =
                reinterpret_cast<void (*)(bool)>(dlsym(RTLD_DEFAULT, "amdva_audit_gl_images")))
            gl(true);
        auto audit_start = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        if (audit_start)
            audit_start();
        std::cout << "Dashboard ready: rtsp://" << options.bind << ":" << options.port
                  << options.mount << " 1920x1080@" << options.fps << " power-max=" << options.power
                  << "W\n"
                  << app.renderer.description() << "\n"
                  << std::flush;
        auto status = g_timeout_add_seconds(
            5,
            [](gpointer p) -> gboolean {
                auto &a = *static_cast<App *>(p);
                ResourceSample s;
                {
                    std::lock_guard<std::mutex> lock(a.mutex);
                    s = a.history.back();
                }
                auto value = [](std::optional<double> v) {
                    return v ? std::to_string(*v) : "null";
                };
                std::cout << "{\"frames\":" << a.frames << ",\"encoder_inputs\":" << a.accepted
                          << ",\"skipped\":" << a.skipped << ",\"cpu_pct\":" << value(s.cpu_pct)
                          << ",\"gpu_pct\":" << value(s.gpu_pct)
                          << ",\"vcn_pct\":" << value(s.vcn_pct)
                          << ",\"vpe_pct\":" << value(s.vpe_pct)
                          << ",\"socket_w\":" << value(s.socket_w) << "}\n"
                          << std::flush;
                return G_SOURCE_CONTINUE;
            },
            &app);
        if (!app.failed)
            g_main_loop_run(app.loop);
        g_source_remove(status);
        app.running = false;
        std::vector<std::shared_ptr<Media>> active;
        {
            std::lock_guard<std::mutex> lock(app.media_mutex);
            for (auto &w : app.media)
                if (auto m = w.lock())
                    active.push_back(m);
        }
        for (auto &m : active)
            m->stop();
        server.disconnect_clients();
        for (auto &m : active)
            m->unprepare();
        g_source_remove(sigint);
        g_source_remove(sigterm);
        if (timer)
            g_source_remove(timer);
        g_main_loop_unref(app.loop);
        app.loop = nullptr;
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        if (errors && errors())
            app.failed = true;
        std::cout << "frames=" << app.frames << " encoder-inputs=" << app.accepted
                  << " skipped=" << app.skipped << " audit-errors=" << (errors ? errors() : 0)
                  << "\n";
        if (auto stop = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_stop")))
            stop();
        return app.failed ? 2 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
