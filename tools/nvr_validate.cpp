#include "window.hpp"
#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSurfaceFormat>
#include <QTimer>
#include <algorithm>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <malloc.h>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: amdva-nvr-validate CONFIG.json SECONDS [CYCLES]\n";
        return 1;
    }
    const std::string path = argv[1];
    const unsigned duration = std::stoul(argv[2]), cycles = argc == 4 ? std::stoul(argv[3]) : 0;
    amdva::nvr::initialize_runtime();
    QSurfaceFormat f;
    f.setRenderableType(QSurfaceFormat::OpenGLES);
    f.setVersion(3, 0);
    f.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(f);
    QApplication app(argc, argv);
    install_shutdown_signals(app);
    int failure = 0;
    try {
        auto options = load_config(path);
        if (options.sources.size() < 20)
            throw std::runtime_error("Validator requires 20 channels");
        const unsigned expected_selected =
            std::count_if(options.sources.begin(), options.sources.end(),
                          [](const auto &s) { return s.selected; });
        Controller controller(options);
        Tiles tiles(controller);
        tiles.resize(1280, 720);
        tiles.show();
        const auto start = Clock::now();
        auto *audit = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        bool auditing = false;
        auto *errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        auto *copies =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_copies"));
        auto *layouts = reinterpret_cast<unsigned long long (*)()>(
            dlsym(RTLD_DEFAULT, "amdva_audit_layout_queries"));
        auto *bytes =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_bytes"));
        unsigned phase = 0;
        bool screenshot = false, rejection = false, initial_checked = false;
        QTimer timer, render_timer;
        QObject::connect(&render_timer, &QTimer::timeout, [&] { tiles.update(); });
        render_timer.start(33);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            if (!auditing && controller.inference_ready()) {
                if (audit)
                    audit();
                auditing = true;
            }
            tiles.update();
            const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            const auto states = controller.status();
            unsigned live = 0, selected = 0, recording = 0;
            uint64_t decoded = 0, inferred = 0, packets = 0, gaps = 0;
            QJsonArray channels;
            for (const auto &s : states) {
                live += s.connection == "live";
                selected += s.slot >= 0;
                recording += s.recording == "recording";
                decoded += s.decoded;
                inferred += s.ai;
                packets += s.received;
                gaps += s.gaps;
                QJsonArray detections;
                for (const auto &d : s.background_detections)
                    detections.append(QJsonObject{{"class", int(d.class_id)},
                                                  {"score", d.score},
                                                  {"x", d.x},
                                                  {"y", d.y},
                                                  {"w", d.w},
                                                  {"h", d.h}});
                channels.append(
                    QJsonObject{{"id", QString::fromStdString(s.source.id)},
                                {"live", s.connection == "live"},
                                {"recording", QString::fromStdString(s.recording)},
                                {"slot", s.slot},
                                {"decoded", double(s.decoded)},
                                {"background_state", QString::fromStdString(s.background_state)},
                                {"background_ai", double(s.background_ai)},
                                {"background_active", s.background_active},
                                {"background_decoded", double(s.background_decoded)},
                                {"background_attempts", double(s.background_attempts)},
                                {"background_capture_us", double(s.background_capture_us)},
                                {"background_age_ms", int(s.background_age_ms)},
                                {"background_detections", detections},
                                {"ai", double(s.ai)},
                                {"display_fps", s.display_fps},
                                {"ai_fps", s.ai_fps},
                                {"generation", double(s.generation)},
                                {"error", QString::fromStdString(s.error)}});
            }
            const auto publication=controller.publication_status();
            const auto heap = mallinfo2();
            QJsonObject result{{"results_delivered", double(publication.delivered)},
                               {"results_dropped", double(publication.dropped)},
                               {"results_errors", double(publication.errors)},
                               {"heap_live_bytes", double(heap.uordblks)},
                               {"heap_free_bytes", double(heap.fordblks)},
                               {"heap_arena_bytes", double(heap.arena)},
                               {"elapsed_s", elapsed},
                               {"live", int(live)},
                               {"selected", int(selected)},
                               {"recording", int(recording)},
                               {"received", double(packets)},
                               {"decoded", double(decoded)},
                               {"ai", double(inferred)},
                               {"gaps", double(gaps)},
                               {"presented", double(tiles.presented())},
                               {"batch", int(controller.last_batch())},
                               {"batch_ms", controller.batch_ms()},
                               {"copy_errors", double(errors ? errors() : 0)},
                               {"allocator_layout_queries", double(layouts ? layouts() : 0)},
                               {"copy_calls", double(copies ? copies() : 0)},
                               {"copy_bytes", double(bytes ? bytes() : 0)},
                               {"channels", channels}};
            std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString() << "\n"
                      << std::flush;
            if (!controller.fatal().empty() || !tiles.error().empty() || (errors && errors())) {
                std::cerr << controller.fatal() << " " << tiles.error() << "\n";
                failure = 2;
                app.quit();
                return;
            }
            if (!screenshot && elapsed > 10 && tiles.presented() > 16) {
                tiles.grabFramebuffer().save(QString::fromStdString(path + ".png"));
                screenshot = true;
            }
            if (expected_selected != 16 && phase == 0) {
                phase = 2;
                rejection = true;
            }
            if (elapsed > 15 && phase == 0) {
                controller.select(options.sources[16].id, true);
                phase++;
            }
            if (phase == 1 && elapsed > 17) {
                for (const auto &s : states)
                    if (s.source.id == options.sources[16].id) {
                        if (s.slot >= 0 || selected != 16) {
                            failure = 3;
                            app.quit();
                            return;
                        }
                        rejection = s.error == "Maximum 16 displayed channels";
                    }
                phase++;
            }
            const unsigned step = elapsed > 20 ? unsigned((elapsed - 20) / 0.5) : 0;
            if (phase >= 2 && phase < 2 + cycles * 4 && step >= phase - 2) {
                const unsigned action = (phase - 2) % 4;
                if (action == 0) {
                    controller.select(options.sources[15].id, false);
                    controller.remove(options.sources[19].id);
                }
                if (action == 1) {
                    controller.select(options.sources[16].id, true);
                    controller.add(options.sources[19]);
                }
                if (action == 2)
                    controller.select(options.sources[16].id, false);
                if (action == 3)
                    controller.select(options.sources[15].id, true);
                phase++;
            }
            if (elapsed > 30 && cycles == 0 && !initial_checked) {
                initial_checked = true;
                if (live != 20 || recording != 20 || selected != expected_selected ||
                    (expected_selected && (!inferred || !tiles.presented())) || !rejection) {
                    failure = 4;
                    app.quit();
                    return;
                }
            }
            if (elapsed >= duration) {
                if (options.background_ai_enabled)
                    for (const auto &s : states)
                        if (s.slot < 0 && !s.background_ai)
                            failure = 6;
                if (live != 20 || selected != expected_selected || recording != 20 || !rejection ||
                    phase < 2 + cycles * 4)
                    failure = 5;
                app.quit();
            }
        });
        timer.start(500);
        const int rc = app.exec();
        controller.shutdown();
        if (audit) {
            auto *stop = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_stop"));
            if (stop)
                stop();
        }
        return failure ? failure : rc;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
