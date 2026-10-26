#include "window.hpp"
#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>
#include <algorithm>
#include <iostream>
#include <sys/stat.h>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: amdva-nvr-faults CONFIG.json\n";
        return 1;
    }
    amdva::nvr::initialize_runtime();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    install_shutdown_signals(app);
    int result = 0;
    try {
        auto options = load_config(argv[1]);
        options.sources.resize(1);
        options.sources[0].selected = true;
        options.segment_seconds = 2;
        Controller controller(options);
        Tiles tiles(controller);
        tiles.resize(640, 360);
        tiles.show();
        QTimer render, timer;
        QObject::connect(&render, &QTimer::timeout, [&] { tiles.update(); });
        render.start(33);
        const auto start = Clock::now();
        unsigned phase = 0;
        uint64_t before = 0;
        bool saw_error = false, secret_safe = true, removed = false;
        std::string cause;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            const auto states = controller.status();
            if (!controller.fatal().empty() || !tiles.error().empty()) {
                result = 2;
                cause = controller.fatal() + tiles.error();
                app.quit();
                return;
            }
            for (const auto &s : states)
                if (s.source.id == "invalid-source" &&
                    s.error.find("secret-marker") != std::string::npos)
                    secret_safe = false;
            if (phase == 0 && elapsed > 8) {
                tiles.resize(64, 64);
                controller.add({"invalid-source", "Unavailable",
                                "rtsp://user:secret-marker@127.0.0.1:1/missing", false});
                phase++;
            }
            if (phase == 1 && elapsed > 12) {
                controller.remove("invalid-source");
                tiles.resize(640, 360);
                phase++;
            }
            if (phase == 2 && elapsed > 15) {
                removed = std::none_of(states.begin(), states.end(), [](const auto &s) {
                    return s.source.id == "invalid-source";
                });
                if (states.size() != 1 || states[0].connection != "live" ||
                    states[0].recording != "recording") {
                    result = 3;
                    cause = "healthy source did not survive failed source";
                    app.quit();
                    return;
                }
                before = states[0].decoded;
                if (chmod(options.recording_root.c_str(), 0000) != 0) {
                    result = 4;
                    cause = "chmod failed";
                    app.quit();
                    return;
                }
                phase++;
            }
            if (phase == 3 && elapsed > 20) {
                if (!states.empty())
                    saw_error |= states[0].recording != "recording";
            }
            if (phase == 3 && elapsed > 25) {
                if (states.empty() || states[0].decoded <= before ||
                    states[0].connection != "live") {
                    result = 5;
                    cause = "storage fault interrupted display/receive";
                    app.quit();
                    return;
                }
                chmod(options.recording_root.c_str(), 0700);
                phase++;
            }
            if (phase == 4 && elapsed > 35) {
                if (states.size() != 1 || states[0].recording != "recording" || !saw_error ||
                    !secret_safe || !removed) {
                    result = 6;
                    cause = "storage or source recovery assertion";
                }
                app.quit();
            }
        });
        timer.start(200);
        app.exec();
        chmod(options.recording_root.c_str(), 0700);
        controller.shutdown();
        std::cout << "{\"passed\":" << (result ? "false" : "true")
                  << ",\"storage_error_visible\":" << (saw_error ? "true" : "false")
                  << ",\"credential_masking\":" << (secret_safe ? "true" : "false")
                  << ",\"source_removed\":" << (removed ? "true" : "false") << "}\n";
        if (result)
            std::cerr << cause << "\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
    return result;
}
