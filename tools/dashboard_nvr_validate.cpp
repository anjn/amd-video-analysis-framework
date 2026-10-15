#include "window.hpp"
#include <QApplication>
#include <QMetaObject>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <chrono>
#include <iostream>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: amdva-dashboard-nvr-validate CONFIG.json\n";
        return 2;
    }
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
        if (options.sources.size() != 1)
            throw std::runtime_error("Dashboard validator requires one source");
        Controller controller(options);
        Window window(controller, options, argv[1]);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("vms-tabs");
        auto *rows = window.findChild<QTableWidget *>("recording-list");
        bool live = false, opened = false, played = false;
        const auto start = std::chrono::steady_clock::now();
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] {
            try {
                const auto seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (!controller.fatal().empty())
                    throw std::runtime_error(controller.fatal());
                if (!window.live_view()->error().empty())
                    throw std::runtime_error(window.live_view()->error());
                if (!window.recording_view()->error().empty())
                    throw std::runtime_error(window.recording_view()->error());
                if (seconds > 12 && !live) {
                    auto status = controller.status();
                    if (status.size() != 1 || !status[0].received || !status[0].decoded ||
                        !status[0].ai || !window.live_view()->presented())
                        throw std::runtime_error("NVR did not decode/display/analyze dashboard");
                    window.live_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[1]) + "-live.png"));
                    live = true;
                    tabs->setCurrentIndex(1);
                }
                if (seconds > 18 && !opened) {
                    for (int i = 0; i < rows->rowCount(); i++)
                        if (rows->item(i, 4)->text() == "finalized") {
                            rows->setCurrentCell(i, 0);
                            QMetaObject::invokeMethod(rows, "cellDoubleClicked", Q_ARG(int, i),
                                                      Q_ARG(int, 0));
                            opened = true;
                            break;
                        }
                }
                if (opened && window.playback_status().frames > 10 && !played) {
                    window.recording_view()->grabFramebuffer().save(
                        QString::fromStdString(std::string(argv[1]) + "-playback.png"));
                    played = true;
                }
                if (window.playback_status().state == "error")
                    throw std::runtime_error(window.playback_status().error);
                if (seconds >= 32) {
                    if (!live || !opened || !played)
                        throw std::runtime_error("Incomplete NVR dashboard scenarios");
                    std::cout << "NVR live decode/display/AI, recording catalog and hardware "
                                 "playback passed\n";
                    app.quit();
                }
            } catch (const std::exception &e) {
                std::cerr << e.what() << "\n";
                failed = 1;
                app.quit();
            }
        });
        timer.start(500);
        app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
    return failed;
}
