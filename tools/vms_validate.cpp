#include "window.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: amdva-vms-validate CONFIG SECONDS\n";
        return 1;
    }
    initialize_runtime();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    int failed = 0;
    bool auditing = false;
    auto audit_start = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
    auto audit_errors =
        reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
    try {
        auto options = load_config(argv[1]);
        Controller controller(options);
        Window window(controller, options, argv[1]);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("vms-tabs");
        auto *layout = window.findChild<QComboBox *>("live-layout");
        auto *table = window.findChild<QTableWidget *>("recording-list");
        auto *camera_table = window.findChild<QTableWidget *>("camera-list");
        auto *assignment = window.findChild<QComboBox *>("tile-camera");
        auto assign = [&](const QString &id) {
            const int index = assignment->findData(id);
            if (index < 0)
                throw std::runtime_error("Missing assignment camera");
            assignment->setCurrentIndex(index);
            QMetaObject::invokeMethod(assignment, "activated", Q_ARG(int, index));
        };
        auto button = [&](const char *id) {
            auto *b = window.findChild<QPushButton *>(id);
            if (!b)
                throw std::runtime_error("Missing playback control");
            b->click();
        };
        auto require = [](bool ok, const char *message) {
            if (!ok)
                throw std::runtime_error(message);
        };
        const auto started = Clock::now();
        int phase = 0;
        std::map<std::string, std::pair<uint64_t, uint64_t>> before;
        uint64_t rename_generation = 0;
        std::string temporary_camera;
        auto camera_row = [&](const std::string &id) {
            for (int i = 0; i < camera_table->rowCount(); i++)
                if (camera_table->item(i, 1)->data(Qt::UserRole).toString().toStdString() == id)
                    return i;
            return -1;
        };
        auto fill_dialog = [&](const QString &name, const QString &uri) {
            QTimer::singleShot(30, [name, uri] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto fields = dialog->findChildren<QLineEdit *>();
                if (fields.size() >= 2) {
                    fields[0]->setText(name);
                    fields[1]->setText(uri);
                    dialog->accept();
                }
            });
        };
        std::string original_uri;
        unsigned paused_frames = 0;
        int64_t paused_position = 0;
        bool live_capture = false, play_capture = false;
        bool saw_h264 = false, saw_h265 = false, saw_continuation = false;
        std::string first_recording;
        auto open_camera = [&](const std::string &id) {
            for (int row = 0; row < table->rowCount(); row++) {
                const auto name = table->item(row, 5)->text().toStdString();
                if (table->item(row, 4)->text() == "finalized" &&
                    name.find("-" + id + "-") != std::string::npos) {
                    table->setCurrentCell(row, 0);
                    QMetaObject::invokeMethod(table, "cellDoubleClicked", Q_ARG(int, row),
                                              Q_ARG(int, 0));
                    return true;
                }
            }
            return false;
        };
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            try {
                if (!auditing && controller.inference_ready()) {
                    if (audit_start)
                        audit_start();
                    auditing = true;
                }
                double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                auto state = window.playback_status();
                auto statuses = controller.status();
                require(window.live_view()->error().empty(), "Live renderer error");
                require(window.recording_view()->error().empty(), "Playback renderer error");
                require(controller.fatal().empty(), "Controller fatal error");
                require(state.state != "error", "Playback error");
                if (!live_capture && elapsed >= 12) {
                    window.live_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[1]) + "-live-frame.png"));
                    window.grab().save(
                        QString::fromStdString(std::string(argv[1]) + "-live-ui.png"));
                    live_capture = true;
                }
                if (!play_capture && elapsed >= 28) {
                    window.recording_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[1]) + "-playback-frame.png"));
                    play_capture = true;
                }
                if (phase == 0 && elapsed >= 15) {
                    require(statuses.size() == options.sources.size(), "Camera registry");
                    unsigned selected = 0;
                    for (auto &s : statuses)
                        if (s.source.selected) {
                            selected++;
                            before[s.source.id] = {s.decoded, s.ai};
                        }
                    require(selected == 16, "Initial fixed 16-view mapping");
                    tabs->setCurrentIndex(1);
                    phase++;
                } else if (phase == 1 && elapsed >= 18) {
                    require(open_camera("cam0"), "H264 recording listed");
                    phase++;
                } else if (phase == 2 && elapsed >= 22) {
                    require(state.frames > 0 && state.duration_ns > 0, "H264 hardware playback");
                    saw_h264 = true;
                    first_recording = state.recording;
                    phase++;
                } else if (phase == 3 && elapsed >= 34) {
                    for (auto &s : statuses)
                        if (before.count(s.source.id))
                            require(s.decoded > before[s.source.id].first &&
                                        s.ai > before[s.source.id].second,
                                    "Live decode/AI stopped during playback");
                    require(state.recording != first_recording,
                            "Adjacent recordings did not continue");
                    saw_continuation = true;
                    button("playback-pause");
                    phase++;
                } else if (phase == 4 && elapsed >= 36) {
                    require(state.state == "paused", "Pause state");
                    paused_frames = state.frames;
                    paused_position = state.position_ns;
                    phase++;
                } else if (phase == 5 && elapsed >= 38) {
                    require(state.frames == paused_frames &&
                                std::abs(state.position_ns - paused_position) < 100000000,
                            "Paused position drift");
                    auto *seek = window.findChild<QSlider *>("playback-seek");
                    seek->setValue(5000);
                    QMetaObject::invokeMethod(seek, "sliderReleased");
                    phase++;
                } else if (phase == 6 && elapsed >= 40) {
                    require(state.state == "paused" && state.frames > paused_frames,
                            "Paused seek/preroll");
                    button("playback-play");
                    phase++;
                } else if (phase == 7 && elapsed >= 43) {
                    auto *filter = window.findChild<QComboBox *>("recording-camera");
                    filter->setCurrentIndex(filter->findData("cam1"));
                    QMetaObject::invokeMethod(filter, "activated",
                                              Q_ARG(int, filter->currentIndex()));
                    phase++;
                } else if (phase == 8 && elapsed >= 46) {
                    require(open_camera("cam1"), "H265 recording listed");
                    phase++;
                } else if (phase == 9 && elapsed >= 50) {
                    require(state.frames > 0 && state.recording.find("-cam1-") != std::string::npos,
                            "H265 hardware playback");
                    saw_h265 = true;
                    tabs->setCurrentIndex(0);
                    window.live_view()->activated(0, true);
                    phase++;
                } else if (phase == 10 && elapsed >= 54) {
                    unsigned selected = 0;
                    for (auto &s : statuses)
                        selected += s.source.selected;
                    require(selected == 16, "Focus changed decode selection");
                    window.live_view()->activated(0, true);
                    layout->setCurrentIndex(layout->findData(4));
                    phase++;
                } else if (phase == 11 && elapsed >= 58) {
                    unsigned selected = 0;
                    for (auto &s : statuses)
                        selected += s.source.selected;
                    require(selected == 4, "4-view layout selection");
                    layout->setCurrentIndex(layout->findData(16));
                    phase++;
                } else if (phase == 12 && elapsed >= 62) {
                    unsigned selected = 0;
                    for (auto &s : statuses)
                        selected += s.source.selected;
                    require(selected == 16, "16-view mapping restore");
                    for (auto &s : statuses)
                        if (s.source.id == "cam0") {
                            auto source = s.source;
                            original_uri = source.uri;
                            rename_generation = s.generation;
                            tabs->setCurrentIndex(2);
                            camera_table->setCurrentCell(camera_row("cam0"), 1);
                            fill_dialog("Renamed entrance", QString::fromStdString(source.uri));
                            button("camera-edit");
                        }
                    phase++;
                } else if (phase == 13 && elapsed >= 66) {
                    for (auto &s : statuses)
                        if (s.source.id == "cam0") {
                            require(s.source.name == "Renamed entrance" &&
                                        s.generation == rename_generation,
                                    "Rename restarted camera");
                            auto source = s.source;
                            source.uri = options.sources[2].uri;
                            controller.update_source(source);
                        }
                    phase++;
                } else if (phase == 14 && elapsed >= 72) {
                    for (auto &s : statuses)
                        if (s.source.id == "cam0") {
                            require(s.generation != rename_generation && s.connection == "live",
                                    "URL edit reconnect");
                            auto source = s.source;
                            source.uri = original_uri;
                            controller.update_source(source);
                        }
                    phase++;
                } else if (phase == 15 && elapsed >= 80) {
                    tabs->setCurrentIndex(1);
                    button("playback-play");
                    phase++;
                }
                if (phase == 16 && elapsed >= 84) {
                    tabs->setCurrentIndex(2);
                    fill_dialog("Temporary test camera",
                                QString::fromStdString(options.sources[19].uri));
                    button("camera-add");
                    phase++;
                } else if (phase == 17 && elapsed >= 90) {
                    require(statuses.size() == 21, "GUI add camera");
                    for (const auto &s : statuses)
                        if (s.source.name == "Temporary test camera")
                            temporary_camera = s.source.id;
                    require(!temporary_camera.empty(), "Added camera ID");
                    RecordingQuery q;
                    q.channel = temporary_camera;
                    require(!controller.recordings(q).empty(), "New camera did not record");
                    camera_table->setCurrentCell(camera_row(temporary_camera), 1);
                    QTimer::singleShot(30, [] {
                        if (auto *dialog =
                                qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                            dialog->button(QMessageBox::Yes)->click();
                    });
                    button("camera-remove");
                    phase++;
                } else if (phase == 18 && elapsed >= 98) {
                    require(statuses.size() == 20, "GUI remove camera");
                    RecordingQuery q;
                    q.channel = temporary_camera;
                    require(!controller.recordings(q).empty(), "Camera removal deleted history");
                    tabs->setCurrentIndex(0);
                    window.live_view()->activated(0, false);
                    assign("cam16");
                    phase++;
                } else if (phase == 19 && elapsed >= 104) {
                    unsigned selected = 0;
                    bool replacement = false;
                    for (const auto &s : statuses) {
                        selected += s.source.selected;
                        if (s.source.id == "cam16")
                            replacement = s.source.selected;
                    }
                    require(selected == 16 && replacement, "Replacement at 16-view limit");
                    assign("cam0");
                    phase++;
                } else if (phase == 20 && elapsed >= 108) {
                    unsigned selected = 0;
                    bool restored = false;
                    for (const auto &s : statuses) {
                        selected += s.source.selected;
                        if (s.source.id == "cam0")
                            restored = s.source.selected;
                    }
                    require(selected == 16 && restored, "Restore replaced camera");
                    assign("cam1");
                    phase++;
                } else if (phase == 21 && elapsed >= 112) {
                    QFile file(QString::fromStdString(std::string(argv[1]) + ".ui.json"));
                    require(file.open(QIODevice::ReadOnly), "Missing persisted layout");
                    auto cells =
                        QJsonDocument::fromJson(file.readAll()).object()["cells"].toArray();
                    require(cells.size() == 16 && cells[0].toString() == "cam1" &&
                                cells[1].toString() == "cam0",
                            "Duplicate assignment did not swap/persist");
                    assign("cam0");
                    phase++;
                } else if (phase == 22 && elapsed >= 116) {
                    button("tile-clear");
                    phase++;
                } else if (phase == 23 && elapsed >= 120) {
                    unsigned selected = 0;
                    for (const auto &s : statuses)
                        selected += s.source.selected;
                    require(selected == 15, "Clear tile did not deselect");
                    assign("cam0");
                    phase++;
                } else if (phase == 24 && elapsed >= 124) {
                    unsigned selected = 0;
                    for (const auto &s : statuses)
                        selected += s.source.selected;
                    require(selected == 16, "Reassign empty tile");
                    tabs->setCurrentIndex(1);
                    phase++;
                }
                if (phase >= 25 && int(elapsed) % 30 == 0)
                    tabs->setCurrentIndex((int(elapsed) / 30) % 2);
                if (phase >= 25 && tabs->currentIndex() == 1 && state.state == "paused")
                    button("playback-play");
                if (phase >= 25 && tabs->currentIndex() == 1 && state.state == "ended" &&
                    int(elapsed) % 10 == 0)
                    button("playback-next");
                QJsonArray channels;
                for (auto &s : statuses)
                    channels.push_back(QJsonObject{{"id", QString::fromStdString(s.source.id)},
                                                   {"received", double(s.received)},
                                                   {"decoded", double(s.decoded)},
                                                   {"ai", double(s.ai)},
                                                   {"live", s.connection == "live"},
                                                   {"selected", s.source.selected}});
                auto json = QJsonObject{
                    {"elapsed_s", elapsed},
                    {"phase", phase},
                    {"channels", channels},
                    {"playback", QJsonObject{{"state", QString::fromStdString(state.state)},
                                             {"recording", QString::fromStdString(state.recording)},
                                             {"frames", double(state.frames)},
                                             {"position_ns", double(state.position_ns)}}}};
                std::cout << QJsonDocument(json).toJson(QJsonDocument::Compact).constData()
                          << std::endl;
                if (elapsed >= std::stoi(argv[2])) {
                    require(phase >= 25 && saw_h264 && saw_h265 && saw_continuation,
                            "Incomplete VMS scenarios");
                    window.grab().save(QString::fromStdString(std::string(argv[1]) + ".png"));
                    app.quit();
                }
            } catch (const std::exception &e) {
                std::cerr << e.what() << "\n";
                failed = 1;
                app.quit();
            }
        });
        timer.start(1000);
        install_shutdown_signals(app);
        app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
    if (audit_errors && audit_errors()) {
        std::cerr << "Copy audit errors: " << audit_errors() << "\n";
        failed = 1;
    }
    if (auto stop = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_stop")))
        stop();
    return failed;
}
