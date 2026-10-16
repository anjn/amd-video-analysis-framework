#include "timeline.hpp"
#include "window.hpp"
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <dlfcn.h>
#include <iostream>
using namespace amdva::nvr;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: amdva-vms-validate CONFIG SECONDS (at least 65)\n";
        return 2;
    }
    const int seconds = std::stoi(argv[2]);
    if (seconds < 65)
        return 2;
    initialize_runtime();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    int failed = 0;
    try {
        auto options = load_config(argv[1]);
        Controller controller(options);
        Window window(controller, options, argv[1]);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("vms-tabs");
        auto *camera = window.findChild<QComboBox *>("recording-camera");
        auto *timeline =
            dynamic_cast<RecordingTimeline *>(window.findChild<QWidget *>("recording-timeline"));
        require(tabs && camera && timeline, "Missing timeline UI");
        require(!window.findChild<QTableWidget *>("recording-list") &&
                    !window.findChild<QWidget *>("playback-seek"),
                "File-based controls remain");
        auto button = [&](const char *id) {
            auto *b = window.findChild<QPushButton *>(id);
            require(b, "Missing playback button");
            b->click();
        };
        auto seek_at = [&](double fraction) {
            double x = 12 + fraction * (timeline->width() - 24);
            for (auto type :
                 {QEvent::MouseMove, QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                auto b = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
                QMouseEvent event(type, QPointF(x, 48), QPointF(x, 48), b,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(timeline, &event);
            }
        };
        auto audit_start = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        auto audit_errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        bool auditing = false, live = false, opened = false, paused = false, sought = false,
             resumed = false, switched = false;
        int64_t switch_time = 0;
        QString switched_camera;
        uint64_t paused_frames = 0, last_presented = 0;
        double max_present_gap_ms = 0;
        bool measuring = false;
        auto last_swap = Clock::now();
        QObject::connect(window.recording_view(), &QOpenGLWindow::frameSwapped, [&] {
            const auto count = window.recording_view()->presented();
            const auto now = Clock::now();
            if (window.timeline_status().state != "playing") {
                measuring = false;
            } else if (count != last_presented) {
                if (measuring)
                    max_present_gap_ms = std::max(
                        max_present_gap_ms,
                        std::chrono::duration<double, std::milli>(now - last_swap).count());
                measuring = true;
                last_swap = now;
            }
            last_presented = count;
        });
        auto started = Clock::now();
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            try {
                auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                require(controller.fatal().empty(), controller.fatal().c_str());
                require(window.live_view()->error().empty(), window.live_view()->error().c_str());
                require(window.recording_view()->error().empty(),
                        window.recording_view()->error().c_str());
                auto state = window.timeline_status();
                require(state.state != "error", state.error.c_str());
                if (elapsed > 5 && !auditing) {
                    if (audit_start)
                        audit_start();
                    auditing = true;
                }
                if (elapsed > 12 && !live) {
                    auto states = controller.status();
                    require(!states.empty(), "No cameras");
                    for (const auto &s : states)
                        require(s.received && (!s.source.selected || (s.decoded && s.ai)),
                                "Live reception/decode/AI failed");
                    require(window.live_view()->presented() > 0, "Live display failed");
                    window.live_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[1]) + "-live-frame.png"));
                    tabs->setCurrentIndex(1);
                    live = true;
                }
                if (elapsed > 23 && !opened) {
                    require(camera->count() > 0 && timeline->oldest() < timeline->latest(),
                            "No camera coverage");
                    require(state.channel == camera->currentData().toString().toStdString() &&
                                state.state == "paused" && state.frames > 0 &&
                                window.recording_view()->presented() > 0,
                            "Initial camera selection did not show the selected-time image");
                    button("playback-play");
                    opened = true;
                }
                if (elapsed > 31 && !paused) {
                    require(state.frames > 30 && window.recording_view()->presented() > 20,
                            "Timeline playback failed");
                    button("playback-pause");
                    paused = true;
                    paused_frames = state.frames;
                }
                if (elapsed > 33 && !sought) {
                    require(state.state == "paused" && state.frames <= paused_frames + 2,
                            "Pause failed");
                    seek_at(.25);
                    sought = true;
                }
                if (elapsed > 34 && sought && !switched && camera->count() > 1) {
                    switch_time = state.position_ms;
                    camera->setCurrentIndex((camera->currentIndex() + 1) % camera->count());
                    switched_camera = camera->currentData().toString();
                    QMetaObject::invokeMethod(camera, "activated",
                                              Q_ARG(int, camera->currentIndex()));
                    switched = true;
                }
                if (elapsed > 36 && !resumed) {
                    if (switched) {
                        require(state.channel == switched_camera.toStdString() &&
                                    std::abs(state.requested_ms - switch_time) < 2 &&
                                    std::abs(state.position_ms - switch_time) < 100,
                                "Camera change did not preserve selected wall time");
                    }
                    require(state.state == "paused" && state.frames > 0,
                            "Paused timeline seek failed");
                    button("playback-play");
                    resumed = true;
                }
                if (elapsed > seconds) {
                    require(state.frames > 60 && state.transitions >= 2,
                            "Continuous playback did not cross fragments");
                    auto rows = controller.status();
                    for (const auto &s : rows)
                        require(s.received && (!s.source.selected || (s.decoded && s.ai)),
                                "Live services stopped during playback");
                    require(max_present_gap_ms < 250, "Playback swap interval exceeded 250ms");
                    if (audit_errors)
                        require(audit_errors() == 0, "CPU pixel copy audit failed");
                    window.grab().save(
                        QString::fromStdString(std::string(argv[1]) + "-timeline-ui.png"));
                    window.recording_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[1]) + "-playback-frame.png"));
                    QJsonObject result{{"elapsed_s", elapsed},
                                       {"frames", qint64(state.frames)},
                                       {"transitions", qint64(state.transitions)},
                                       {"position_ms", qint64(state.position_ms)},
                                       {"camera_switch_preserved_time", switched},
                                       {"max_present_gap_ms", max_present_gap_ms},
                                       {"state", QString::fromStdString(state.state)},
                                       {"audit_errors", qint64(audit_errors ? audit_errors() : 0)}};
                    QFile file(
                        QString::fromStdString(std::string(argv[1]) + "-timeline-results.json"));
                    require(file.open(QIODevice::WriteOnly), "Cannot write results");
                    file.write(QJsonDocument(result).toJson());
                    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString()
                              << std::endl;
                    app.quit();
                }
            } catch (const std::exception &e) {
                std::cerr << e.what() << '\n';
                failed = 1;
                app.quit();
            }
        });
        timer.start(100);
        app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        failed = 1;
    }
    return failed;
}
