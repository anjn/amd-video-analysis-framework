#include "timeline.hpp"
#include "window.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
using namespace amdva::nvr;
void check(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
void click(Tiles *view) {
    auto point = view->displayed_video_rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(view, &press);
    QApplication::sendEvent(view, &release);
}
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: amdva-vms-validate CONFIG SECONDS (>=90)\n";
        return 2;
    }
    initialize_runtime();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    int failed = 0;
    try {
        auto options = load_config(argv[1]);
        Controller controller(options);
        Window window(controller, options, argv[1]);
        window.resize(1280, 900);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("vms-tabs");
        auto *timeline =
            dynamic_cast<RecordingTimeline *>(window.findChild<QWidget *>("recording-timeline"));
        auto *layout = window.findChild<QComboBox *>("live-layout");
        auto *back = window.findChild<QPushButton *>("playback-live");
        check(tabs && timeline && layout && back, "Unified UI missing");
        for (int i = 0; i < tabs->count(); ++i)
            check(tabs->tabText(i) != "Recordings", "Old Recordings tab remains");
        check(!window.findChild<QPushButton *>("playback-pause") &&
                  !window.findChild<QWidget *>("playback-show-time"),
              "Old controls remain");
        auto seek_click = [&](int64_t at) {
            const double x =
                12. + double(timeline->latest() - at) /
                          std::max<int64_t>(1, timeline->latest() - timeline->oldest()) *
                          (timeline->height() - 24);
            QPointF point(24, x);
            QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(timeline, &press);
            QApplication::sendEvent(timeline, &release);
        };
        auto audit = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        auto started = Clock::now();
        const int64_t wall_started = QDateTime::currentMSecsSinceEpoch();
        int stage = 0;
        double since = 0;
        uint64_t held_frames = 0;
        int64_t paused_time = 0;
        bool overlay_seen = false;
        int log_second = -1, cycle = -1;
        uint64_t warmup_tracking_drops = 0;
        int duration = std::max(90, std::stoi(argv[2]));
        std::ofstream log(std::string(argv[1]) + ".validation.log");
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &app, [&] {
            try {
                double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                auto advance = [&] {
                    ++stage;
                    since = elapsed;
                    std::cout << "stage=" << stage << " elapsed=" << elapsed << std::endl;
                };
                check(controller.fatal().empty(), controller.fatal());
                check(window.live_view()->error().empty(), window.live_view()->error());
                check(window.recording_view()->error().empty(), window.recording_view()->error());
                auto state = window.timeline_status();
                check(state.state != "error", state.error);
                if (stage && stage < 14 && elapsed - since > 20)
                    throw std::runtime_error("Stage timeout " + std::to_string(stage));
                if (int(elapsed) / 5 != log_second) {
                    log_second = int(elapsed) / 5;
                    struct rusage usage {};
                    getrusage(RUSAGE_SELF, &usage);
                    log << "elapsed=" << elapsed << " stage=" << stage
                        << " peak_rss_mib=" << usage.ru_maxrss / 1024. << "\n";
                    for (const auto &m : controller.ai_runtime()->snapshot()) {
                        if (m.stage == "Tracking association" || m.stage == "Tracking queue" ||
                            m.stage == "Analysis save queue") {
                            log << m.stage << " camera=" << m.camera << " rate=" << m.rate
                                << " pending=" << m.pending << " dropped=" << m.dropped << "\n";
                            if (elapsed > 80 && m.stage == "Tracking association" &&
                                !m.camera.empty())
                                check(m.rate >= 1.8 && m.rate <= 2.2,
                                      "Tracking rate outside 2 Hz target: " + m.camera);
                            if (m.stage == "Tracking queue") {
                                if (elapsed <= 30)
                                    warmup_tracking_drops = m.dropped;
                                if (elapsed > 30)
                                    check(m.dropped == warmup_tracking_drops,
                                          "Tracking queue dropped frames after warmup");
                            }
                            if (elapsed > 80 && m.stage == "Analysis save queue")
                                check(m.dropped == 0, "Analysis save queue dropped frames");
                        }
                    }
                    log.flush();
                    check(controller.recording_catalog().analysis_error().empty(),
                          controller.recording_catalog().analysis_error());
                }
                if (stage == 0 && elapsed > 15 && controller.inference_ready() &&
                    window.live_view()->presented() > 0) {
                    if (audit)
                        audit();
                    // Widget grabs omit the native video; keep the rendered grid as well,
                    // before the panel toggles below resize the video window.
                    window.live_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[1]) + ".grid-video.png"));
                    for (const auto &line : window.live_view()->displayed_encode_info())
                        if (!line.isEmpty())
                            std::cout << "encode info: " << line.toStdString() << std::endl;
                    // The camera panel starts closed; the boundary button opens and closes
                    // it. Leave it open for the later grid checks.
                    auto *toggle = window.findChild<QAbstractButton *>("live-panel-toggle");
                    auto *panel = window.findChild<QWidget *>("tile-camera-panel");
                    check(toggle && panel && toggle->isVisible() && !panel->isVisible(),
                          "Camera panel did not start closed");
                    window.grab().save(
                        QString::fromStdString(std::string(argv[1]) + ".grid-hidden.png"));
                    toggle->click();
                    check(panel->isVisible(), "Panel toggle did not open");
                    toggle->click();
                    check(!panel->isVisible(), "Panel toggle did not close");
                    toggle->click();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                    window.grab().save(QString::fromStdString(std::string(argv[1]) + ".grid.png"));
                    std::cout << "PASS camera panel toggle" << std::endl;
                    layout->setCurrentIndex(layout->findData(16));
                    QMetaObject::invokeMethod(layout, "activated",
                                              Q_ARG(int, layout->currentIndex()));
                    auto *v = window.live_view();
                    QPointF point(v->width() / 8., v->height() / 8.);
                    QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton,
                                      Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                    QMouseEvent twice(QEvent::MouseButtonDblClick, point, point, Qt::LeftButton,
                                      Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(v, &press);
                    QApplication::sendEvent(v, &release);
                    QApplication::sendEvent(v, &twice);
                    QApplication::sendEvent(v, &release);
                    advance();
                } else if (stage == 1 && timeline->isVisible() && timeline->oldest() &&
                           elapsed - since > 2) {
                    check(window.live_view()->isExposed(), "Single live not exposed");
                    check(!layout->isVisible() &&
                              !window.findChild<QWidget *>("tile-camera-panel")->isVisible() &&
                              !window.findChild<QWidget *>("recording-camera")->isVisible(),
                          "Grid settings visible in single view");
                    window.grab().save(
                        QString::fromStdString(std::string(argv[1]) + ".single.png"));
                    click(window.live_view());
                    advance();
                } else if (stage == 2 && window.live_view()->live_held()) {
                    held_frames = window.live_view()->presented();
                    advance();
                } else if (stage == 3 && elapsed - since > 1) {
                    check(window.live_view()->presented() == held_frames,
                          "Live pause did not freeze frame");
                    window.live_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[1]) + ".paused.png"));
                    timeline->preview_requested(timeline->oldest() + 2000, {});
                    advance();
                } else if (stage == 4 && window.keyframe_preview_status().image &&
                           window.live_view()->displayed_playback_time() > 0) {
                    check(window.live_view()->preview_active(), "Hover not in main view");
                    const auto previous = window.live_view()->displayed_playback_time();
                    window.live_view()->preview_image(true, {});
                    auto retained = window.live_view()->grabFramebuffer();
                    check(!retained.isNull() &&
                              window.live_view()->displayed_playback_time() == previous,
                          "Loading preview discarded the preceding image");
                    timeline->preview_requested({}, {});
                    advance();
                } else if (stage == 5 && elapsed - since > 1) {
                    check(window.live_view()->live_held() &&
                              window.live_view()->presented() == held_frames,
                          "Hover did not restore paused live");
                    click(window.live_view());
                    advance();
                } else if (stage == 6 && !window.live_view()->live_held() && elapsed - since > 1) {
                    check(window.live_view()->presented() > held_frames, "Live did not resume");
                    seek_click(timeline->oldest() + 2000);
                    advance();
                } else if (stage == 7 && window.recording_view()->displayed_playback_time() > 0 &&
                           state.frames > 5 && elapsed - since > 2) {
                    check(!window.recording_view()->preview_active(),
                          "Timeline refresh restarted hover after click");
                    overlay_seen |= window.recording_view()->displayed_analysis_objects() > 0;
                    if (!overlay_seen && elapsed - since < 10)
                        return;
                    check(overlay_seen, "Recorded analysis overlay missing");
                    window.recording_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[1]) + ".recording.png"));
                    if (auto audit_gl = reinterpret_cast<void (*)(bool)>(
                            dlsym(RTLD_DEFAULT, "amdva_audit_gl_images")))
                        audit_gl(true);
                    click(window.recording_view());
                    advance();
                } else if (stage == 8 && state.state == "paused" && elapsed - since > 1) {
                    paused_time = state.position_ms;
                    timeline->preview_requested(timeline->oldest() + 1000, {});
                    advance();
                } else if (stage == 9 && window.keyframe_preview_status().image &&
                           window.recording_view()->preview_active()) {
                    timeline->preview_requested({}, {});
                    advance();
                } else if (stage == 10 && elapsed - since > 1) {
                    check(state.state == "paused" &&
                              std::abs(state.position_ms - paused_time) < 100,
                          "Hover did not restore paused recording");
                    seek_click(timeline->oldest() + 4000);
                    advance();
                } else if (stage == 11 && state.state == "playing" && state.frames > 5) {
                    timeline->preview_requested(timeline->oldest() + 1000, {});
                    advance();
                } else if (stage == 12 && window.keyframe_preview_status().image) {
                    timeline->preview_requested({}, {});
                    advance();
                } else if (stage == 13 && state.state == "playing" && elapsed - since > 1) {
                    back->click();
                    check(!window.live_view()->live_held(), "Back to Live stayed paused");
                    advance();
                } else if (stage == 14) {
                    const int phase = int(elapsed - since) / 15;
                    if (phase != cycle) {
                        cycle = phase;
                        if (phase % 4 == 0) {
                            back->click();
                            // The first cycle leaves single view with Back to Tile, later
                            // cycles with Esc.
                            if (phase == 0) {
                                auto *tile = window.findChild<QPushButton *>("playback-tile");
                                check(tile && tile->isVisible(), "Missing Back to Tile");
                                tile->click();
                            } else {
                                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape,
                                                 Qt::NoModifier);
                                QApplication::sendEvent(window.live_view(), &escape);
                            }
                        }
                        if (phase % 4 == 0) {
                            check(layout->isVisible() &&
                                      window.findChild<QWidget *>("tile-camera-panel")->isVisible(),
                                  "Grid panel not restored");
                            check(
                                window.findChild<QLabel *>("storage-status")->text().count('\n') ==
                                    3,
                                "Storage status must have four lines");
                        }
                        if (phase % 4 == 1)
                            window.live_view()->activated(0, true);
                        if (phase % 4 == 2)
                            timeline->seek_requested(
                                std::max(timeline->oldest() + 2000, timeline->latest() - 15000));
                        if (phase % 4 == 3)
                            timeline->preview_requested(
                                std::max(timeline->oldest() + 1000, timeline->latest() - 10000),
                                {});
                    }
                    if (elapsed > duration) {
                        timeline->preview_requested({}, {});
                        back->click();
                        if (errors)
                            check(errors() == 0, "Copy audit violations");
                        for (const auto &s : controller.status())
                            check(s.error.empty() && s.received > 0,
                                  "Camera failed: " + s.source.id + " " + s.error);
                        const auto until = QDateTime::currentMSecsSinceEpoch() - 2000;
                        for (const auto &source : controller.status()) {
                            const auto stored = controller.recording_catalog().analysis(
                                source.source.id, wall_started + 30000, until);
                            const double rate =
                                stored.size() * 1000. / (until - wall_started - 30000);
                            std::cout << "stored_analysis camera=" << source.source.id
                                      << " rate=" << rate << " frames=" << stored.size()
                                      << std::endl;
                            check(rate >= 1.9 && rate <= 2.1,
                                  "Recorded analysis lost frames at fragment boundaries: " +
                                      source.source.id);
                        }
                        std::cout << "PASS unified live/recording/hover/click/overlay; seconds="
                                  << elapsed << " warmup_tracking_drops=" << warmup_tracking_drops
                                  << std::endl;
                        window.findChild<QPushButton *>("exit-application")->click();
                    }
                }
                if (elapsed > duration + 90)
                    throw std::runtime_error("Validation deadline exceeded");
            } catch (const std::exception &e) {
                std::cerr << "FAIL " << e.what() << std::endl;
                failed = 1;
                app.quit();
            }
        });
        timer.start(100);
        app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return failed;
}
