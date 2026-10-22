#include "window.hpp"
#include <QApplication>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <unistd.h>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: amdva-ai-validate CONFIG SECONDS OUTPUT.jsonl\n";
        return 2;
    }
    initialize_runtime();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(2, 0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    int result = 0;
    try {
        auto options = load_config(argv[1]);
        Controller controller(options);
        Window window(controller, options, argv[1]);
        window.resize(1280, 900);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>("vms-tabs");
        if (!window.findChild<QComboBox *>("ai-frequency") ||
            !window.findChild<QComboBox *>("embedding-interval"))
            throw std::runtime_error("Missing AI controls");
        std::ofstream log(argv[3]);
        if (!log)
            throw std::runtime_error("Cannot open log");
        int seconds = std::stoi(argv[2]);
        if (seconds < 30)
            throw std::runtime_error("Duration must be >=30 sec");
        auto change = [&](const char *name, unsigned value) {
            auto *combo = window.findChild<QComboBox *>(name);
            combo->setCurrentIndex(combo->findData(value));
            QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection,
                                      Q_ARG(int, combo->currentIndex()));
        };
        bool controls_checked = false;
        auto start = Clock::now();
        int mode = -1;
        auto mode_changed = Clock::now();
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, &app, [&] {
            try {
                if (!controller.fatal().empty())
                    throw std::runtime_error(controller.fatal());
                if (!controls_checked) {
                    if (!controller.status_ready())
                        return;
                    change("ai-frequency", 5);
                    change("embedding-interval", 60000);
                    auto saved = load_config(argv[1]);
                    if (controller.ai_runtime()->fps() != 5 || saved.ai_fps != 5 ||
                        saved.search.interval_ms != 60000)
                        throw std::runtime_error("AI controls did not persist");
                    change("ai-frequency", options.ai_fps);
                    change("embedding-interval", std::max(5000u, options.search.interval_ms));
                    controls_checked = true;
                }
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start).count();
                int next = std::min(2, int(elapsed * 3 / seconds));
                if (next != mode) {
                    mode = next;
                    auto *layout = window.findChild<QComboBox *>("live-layout");
                    if (!layout)
                        throw std::runtime_error("Missing live layout control");
                    layout->setCurrentIndex(layout->findData(mode == 0 ? 16 : mode == 1 ? 4 : 1));
                    if (mode == 2) {
                        auto *clear = window.findChild<QPushButton *>("tile-clear");
                        if (!clear)
                            throw std::runtime_error("Missing tile clear control");
                        clear->click();
                    }
                    mode_changed = Clock::now();
                }
                QJsonArray rows, cameras;
                for (const auto &m : controller.ai_runtime()->snapshot())
                    rows.append(QJsonObject{{"stage", QString::fromStdString(m.stage)},
                                            {"camera", QString::fromStdString(m.camera)},
                                            {"mean_ms", m.mean_ms},
                                            {"p95_ms", m.p95_ms},
                                            {"max_ms", m.max_ms},
                                            {"wait_ms", m.wait_ms},
                                            {"rate", m.rate},
                                            {"mean_batch", m.mean_batch},
                                            {"pending", int(m.pending)},
                                            {"dropped", double(m.dropped)}});
                for (const auto &s : controller.status()) {
                    const auto index =
                        std::find_if(options.sources.begin(), options.sources.end(),
                                     [&](const auto &source) { return source.id == s.source.id; }) -
                        options.sources.begin();
                    const bool expected = mode == 0 || (mode == 1 && index < 4);
                    if (Clock::now() - mode_changed > std::chrono::seconds(10) &&
                        s.source.selected != expected)
                        throw std::runtime_error("Visibility mode did not persist: " + s.source.id);
                    double tracking_age = -1;
                    if (auto *search = controller.search_service();
                        search && !options.search.reid_model.empty()) {
                        auto tracked = search->tracks(s.source.id);
                        if (tracked.generation)
                            tracking_age = std::chrono::duration<double, std::milli>(
                                               Clock::now() - tracked.captured)
                                               .count();
                        if (Clock::now() - mode_changed > std::chrono::seconds(10) &&
                            (tracking_age < 0 || tracking_age > 3000))
                            throw std::runtime_error("Stale tracking timestamp: " + s.source.id);
                    }
                    cameras.append(QJsonObject{{"id", QString::fromStdString(s.source.id)},
                                               {"tracking_age_ms", tracking_age},
                                               {"selected", s.source.selected},
                                               {"slot", s.slot},
                                               {"input_width", int(s.input_width)},
                                               {"input_height", int(s.input_height)},
                                               {"received", double(s.received)},
                                               {"ai", double(s.ai)},
                                               {"background_ai", double(s.background_ai)},
                                               {"error", QString::fromStdString(s.error)}});
                }
                auto *search = controller.search_service();
                auto state = search ? search->status() : amdva::streaming::SearchStatus{};
                QJsonObject sample{
                    {"elapsed", double(elapsed)},
                    {"mode", mode},
                    {"metrics", rows},
                    {"cameras", cameras},
                    {"stored", double(state.stored)},
                    {"search_error", QString::fromStdString(state.error)},
                    {"effective_interval_ms", int(controller.ai_runtime()->effective_interval())}};
                size_t pages = 0, resident = 0;
                std::ifstream memory("/proc/self/statm");
                memory >> pages >> resident;
                rusage usage{};
                getrusage(RUSAGE_SELF, &usage);
                sample["rss_mib"] = double(resident) * sysconf(_SC_PAGESIZE) / (1024 * 1024);
                sample["peak_rss_mib"] = double(usage.ru_maxrss) / 1024;
                sample["cpu_seconds"] = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
                                        (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
                log << QJsonDocument(sample).toJson(QJsonDocument::Compact).constData() << '\n';
                log.flush();
                std::cout << "elapsed=" << elapsed << " mode=" << mode << " stored=" << state.stored
                          << " pending=" << state.pending << " error=" << state.error << std::endl;
                if (state.state == "error")
                    throw std::runtime_error(state.error);
                if (elapsed >= seconds) {
                    if (search && state.stored == 0)
                        throw std::runtime_error("No semantic observations stored");
                    for (const auto &source : options.sources) {
                        bool found = false;
                        for (const auto &metric : controller.ai_runtime()->snapshot())
                            if (metric.stage == "Detection update" && metric.camera == source.id &&
                                metric.rate > 0)
                                found = true;
                        if (!found)
                            throw std::runtime_error("Missing inference: " + source.id);
                    }
                    tabs->setCurrentIndex(4);
                    QTimer::singleShot(1500, &app, [&] {
                        window.grab().save(QString::fromStdString(std::string(argv[3]) + ".png"));
                        app.quit();
                    });
                    timer.stop();
                }
            } catch (const std::exception &e) {
                std::cerr << e.what() << '\n';
                result = 1;
                app.quit();
            }
        });
        timer.start(5000);
        app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    return result;
}
