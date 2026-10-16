#include "thumbnail.hpp"
#include "timeline.hpp"
#include "window.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
using namespace amdva::nvr;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: amdva-vms-management-validate CONFIG H264_FIXTURE.mkv\n";
        return 2;
    }
    initialize_runtime();
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setVersion(3, 0);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication app(argc, argv);
    int failed = 0;
    try {
        auto options = load_config(argv[1]);
        check(options.retention_minutes == 0, "Test must start with retention disabled");
        check(!std::filesystem::exists(options.recording_root), "Test recording root must be new");
        {
            Catalog setup(options.recording_root);
            for (auto name : {"expired.mkv", "recent.mkv"}) {
                std::filesystem::copy_file(argv[2],
                                           std::filesystem::path(options.recording_root) / name);
                setup.opened(name, "expiry-fixture");
                setup.media_start(name, QDateTime::currentMSecsSinceEpoch() -
                                            (std::string(name) == "expired.mkv" ? 90000 : 30000));
                setup.closed(name, 2 * GST_SECOND);
            }
        }
        Controller controller(options);
        auto pin = controller.open_recording("expired.mkv");
        Window window(controller, options, argv[1]);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("vms-tabs");
        auto *timeline =
            dynamic_cast<RecordingTimeline *>(window.findChild<QWidget *>("recording-timeline"));
        auto *popup = window.thumbnail_popup();
        auto *retention = window.findChild<QSpinBox *>("recording-retention");
        check(tabs && timeline && popup && retention, "Missing management UI");
        auto button = [&](const char *name) {
            auto *b = window.findChild<QPushButton *>(name);
            check(b, "Missing button");
            b->click();
        };
        auto click_view = [&] {
            auto *view = window.recording_view();
            QMouseEvent event(QEvent::MouseButtonPress, QPointF(40, 40), QPointF(40, 40),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &event);
        };
        auto audit_start = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        auto audit_errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        auto audit_gl =
            reinterpret_cast<void (*)(bool)>(dlsym(RTLD_DEFAULT, "amdva_audit_gl_images"));
        auto cancelled_gate = std::make_shared<std::promise<void>>();
        bool gated = false, cancelled = false, cancel_verified = false;
        bool applied = false, released = false, expired = false, record = false, played = false,
             paused = false, resumed = false, benchmark = false, waiting = false, purged = false,
             finish = false;
        uint64_t expected = 0, last_handled = 0;
        unsigned samples = 0, retained_checks = 0, moved_checks = 0;
        QWindow *preview_window = nullptr;
        for (auto *candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == "seek-preview-window")
                preview_window = candidate;
        check(preview_window, "Missing native preview window");
        std::vector<double> times;
        std::vector<std::string> old_files, active_files;
        auto start = Clock::now(), request_start = start, completed_at = start;
        std::function<void()> next;
        next = [&] {
            auto at = timeline->oldest() + 1000 + int64_t(samples) * 60;
            double x = 12 + double(at - timeline->oldest()) /
                                double(timeline->latest() - timeline->oldest()) *
                                (timeline->width() - 24);
            auto previous = popup->displayed_image();
            const auto previous_position = preview_window->position();
            request_start = Clock::now();
            QMouseEvent event(QEvent::MouseMove, QPointF(x, 48), QPointF(x, 48), Qt::NoButton,
                              Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(timeline, &event);
            auto requested = window.keyframe_preview_status();
            expected = requested.request;
            if (previous && requested.state == "loading") {
                check(popup->displayed_image() == previous,
                      "Loading cleared the previous GPU image");
                check(popup->isVisible(), "Loading hid the popup");
                retained_checks++;
            }
            if (preview_window->position() != previous_position)
                moved_checks++;
        };
        popup->presented = [&](uint64_t id) {
            if (!waiting || id < expected || id == last_handled)
                return;
            auto state = window.keyframe_preview_status();
            if (!state.image || state.request != id)
                return;
            try {
                check(popup->error().empty(), popup->error().c_str());
                check(state.image->frame_ms <= state.requested_ms,
                      "Preview keyframe is after cursor");
                last_handled = id;
                times.push_back(
                    std::chrono::duration<double, std::milli>(Clock::now() - request_start)
                        .count());
                samples++;
                if (samples < 100)
                    QTimer::singleShot(0, next);
                else {
                    check(retained_checks > 0 && moved_checks > 0,
                          "Preview retention/movement not exercised");
                    // Retention must never redisplay a surface invalidated by deletion.
                    auto invalidated = state;
                    invalidated.state = "loading";
                    invalidated.image.reset();
                    invalidated.deletion_generation++;
                    popup->display(invalidated, timeline->mapToGlobal(QPoint(300, 0)));
                    check(!popup->displayed_image(), "Deletion invalidation retained stale pixels");
                    waiting = false;
                    QEvent leave(QEvent::Leave);
                    QApplication::sendEvent(timeline, &leave);
                    std::sort(times.begin(), times.end());
                    std::cout << "preview_samples=100 p95_ms=" << times[94]
                              << " max_ms=" << times.back() << std::endl;
                    completed_at = Clock::now();
                }
            } catch (const std::exception &e) {
                std::cerr << e.what() << '\n';
                failed = 1;
                app.quit();
            }
        };
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            try {
                double seconds = std::chrono::duration<double>(Clock::now() - start).count();
                if (benchmark && waiting && int(seconds * 20) % 100 == 0) {
                    auto ps = window.keyframe_preview_status();
                    std::cout << "progress samples=" << samples << " state=" << ps.state
                              << " id=" << ps.request << " expected=" << expected
                              << " popup=" << popup->isVisible() << " exposed=" << popup->exposed()
                              << " error=" << popup->error() << std::endl;
                }
                check(popup->error().empty(), popup->error().c_str());
                check(seconds < 90, "GUI validation timed out");
                check(controller.fatal().empty(), controller.fatal().c_str());
                if (seconds > 2 && !applied) {
                    retention->setValue(1);
                    button("recording-retention-apply");
                    applied = true;
                }
                if (seconds > 4 && !released) {
                    check(std::filesystem::exists(std::filesystem::path(options.recording_root) /
                                                  "expired.mkv"),
                          "Pinned expired recording deleted");
                    pin.reset();
                    released = true;
                }
                if (seconds > 6 && !expired) {
                    check(!std::filesystem::exists(std::filesystem::path(options.recording_root) /
                                                   "expired.mkv"),
                          "Expiration worker failed");
                    check(std::filesystem::exists(std::filesystem::path(options.recording_root) /
                                                  "recent.mkv"),
                          "Recent recording expired");
                    check(load_config(argv[1]).retention_minutes == 1, "Retention not persisted");
                    expired = true;
                    if (audit_gl)
                        audit_gl(true);
                    if (audit_start)
                        audit_start();
                }
                if (seconds > 8 && !gated) {
                    controller.delete_closed_recordings(cancelled_gate->get_future().share());
                    gated = true;
                }
                if (seconds > 9 && !cancelled) {
                    check(controller.deletion_status().state == "waiting",
                          "Deletion ignored release gate");
                    check(std::filesystem::exists(std::filesystem::path(options.recording_root) /
                                                  "recent.mkv"),
                          "Gated deletion removed file");
                    controller.cancel_recording_deletion();
                    cancelled = true;
                }
                if (seconds > 10 && !cancel_verified) {
                    check(controller.deletion_status().state == "cancelled", "Cancellation failed");
                    check(std::filesystem::exists(std::filesystem::path(options.recording_root) /
                                                  "recent.mkv"),
                          "Cancelled deletion removed file");
                    cancel_verified = true;
                }
                if (seconds > 12 && !record) {
                    auto states = controller.status();
                    for (auto &s : states)
                        check(s.received && s.decoded && s.ai, "Live decode/AI failed");
                    tabs->setCurrentIndex(1);
                    record = true;
                }
                if (seconds > 14 && !played) {
                    check(window.timeline_status().frames > 0, "Initial paused preview absent");
                    click_view();
                    played = true;
                }
                if (seconds > 15 && !paused) {
                    check(window.timeline_status().state == "playing", "Image click did not play");
                    click_view();
                    paused = true;
                }
                if (seconds > 16 && !resumed) {
                    check(window.timeline_status().state == "paused", "Image click did not pause");
                    click_view();
                    resumed = true;
                }
                if (seconds > 17 && !benchmark) {
                    benchmark = true;
                    waiting = true;
                    next();
                }
                if (samples == 100 && !purged &&
                    Clock::now() - completed_at > std::chrono::milliseconds(300)) {
                    RecordingQuery query;
                    query.limit = 200;
                    for (auto &r : controller.recordings(query))
                        if (r.available)
                            (r.state == "recording" ? active_files : old_files).push_back(r.name);
                    check(!old_files.empty() && !active_files.empty(),
                          "No closed/active recording coverage");
                    QTimer::singleShot(0, [&] {
                        for (auto *widget : QApplication::topLevelWidgets())
                            if (auto *box = qobject_cast<QMessageBox *>(widget))
                                box->button(QMessageBox::Yes)->click();
                    });
                    button("recording-delete-all");
                    purged = true;
                }
                if (purged && !finish) {
                    auto state = controller.deletion_status();
                    if (state.state == "complete") {
                        check(state.failed == 0 && state.protected_files == 0,
                              "Deletion incomplete");
                        for (auto &name : old_files)
                            check(!std::filesystem::exists(
                                      std::filesystem::path(options.recording_root) / name),
                                  "Closed file survived delete-all");
                        for (auto &name : active_files)
                            check(std::filesystem::exists(
                                      std::filesystem::path(options.recording_root) / name),
                                  "Active file was deleted");
                        check(window.timeline_status().state == "stopped",
                              "Deletion did not stop playback");
                        check(!window.keyframe_preview_status().image,
                              "Deleted preview still exposed");
                        completed_at = Clock::now();
                        finish = true;
                    }
                }
                if (finish && Clock::now() - completed_at > std::chrono::seconds(2)) {
                    auto states = controller.status();
                    for (auto &s : states)
                        check(s.received > 500 && s.decoded > 300 && s.ai > 10,
                              "Live services interrupted");
                    if (audit_errors)
                        check(audit_errors() == 0, "Copy audit failed");
                    QJsonObject result{{"samples", 100},
                                       {"retained_loading_checks", int(retained_checks)},
                                       {"popup_movement_checks", int(moved_checks)},
                                       {"p50_ms", times[49]},
                                       {"p95_ms", times[94]},
                                       {"max_ms", times.back()},
                                       {"audit_errors", qint64(audit_errors ? audit_errors() : 0)},
                                       {"deleted", qint64(controller.deletion_status().deleted)},
                                       {"expiry", expired},
                                       {"cancel_release_gate", cancel_verified},
                                       {"click_toggle", resumed},
                                       {"active_files_preserved", true}};
                    QFile file(
                        QString::fromStdString(std::string(argv[1]) + "-management-results.json"));
                    check(file.open(QIODevice::WriteOnly), "Cannot write results");
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
        timer.start(50);
        app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        failed = 1;
    }
    return failed;
}
