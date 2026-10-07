#include "window.hpp"
#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>
#include <iostream>
#include <map>
using namespace amdva;
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: nvr-timing-tests CONFIG\n";
        return 1;
    }
    initialize_runtime();
    QSurfaceFormat f;
    f.setRenderableType(QSurfaceFormat::OpenGLES);
    f.setVersion(3, 0);
    f.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(f);
    QApplication app(argc, argv);
    int failed = 0;
    try {
        auto options = load_config(argv[1]);
        options.preview_enabled = true;
        options.telemetry.enabled = true;
        Controller controller(options);
        Tiles tiles(controller);
        tiles.resize(640, 360);
        tiles.show();
        std::map<std::string, std::pair<uint64_t, uint64_t>> before;
        unsigned phase = 0;
        QTimer render, timer;
        QObject::connect(&render, &QTimer::timeout, [&] { tiles.update(); });
        render.start(5);
        const auto start = Clock::now();
        auto require = [&](bool ok, const char *text) {
            if (!ok) {
                std::cerr << text << '\n';
                failed = 1;
                app.quit();
            }
        };
        QObject::connect(&timer, &QTimer::timeout, [&] {
            if (!controller.fatal().empty() || !tiles.error().empty()) {
                require(false, "Controller/renderer error");
                return;
            }
            const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            const auto metrics = controller.telemetry();
            if (phase == 0 && elapsed >= 10) {
                for (const auto &s : metrics) {
                    auto decode = s.stages[0][unsigned(TimingStage::DecodeOutput)].count,
                         swap = s.stages[0][unsigned(TimingStage::Swap)].count;
                    require(decode > 30 && swap > 30, "Initial decode/swap observations");
                    before[s.source] = {decode, swap};
                }
                tiles.hide();
                phase++;
            } else if (phase == 1 && elapsed >= 13) {
                for (const auto &s : metrics) {
                    const auto &swap = s.stages[0][unsigned(TimingStage::Swap)];
                    require(!swap.active && !swap.stalled, "Hidden display must be paused");
                    require(swap.count == before[s.source].second,
                            "Hidden display must not accumulate presentations");
                    require(s.stages[0][unsigned(TimingStage::DecodeOutput)].count >
                                before[s.source].first + 30,
                            "Decode continues while hidden");
                }
                tiles.show();
                phase++;
            } else if (phase == 2 && elapsed >= 17) {
                for (const auto &s : metrics)
                    require(s.stages[0][unsigned(TimingStage::Swap)].count >
                                before[s.source].second + 30,
                            "Presentation resumes");
                tiles.setWindowState(Qt::WindowMinimized);
                phase++;
            } else if (phase == 3 && elapsed >= 20) {
                for (const auto &s : metrics)
                    require(!s.stages[0][unsigned(TimingStage::Swap)].active,
                            "Minimized display must be paused");
                tiles.hide();
                tiles.setWindowState(Qt::WindowNoState);
                tiles.show();
                tiles.resize(320, 240);
                phase++;
            } else if (phase == 4 && elapsed >= 24) {
                for (const auto &s : metrics) {
                    const auto &decode = s.stages[0][unsigned(TimingStage::DecodeOutput)],
                               &swap = s.stages[0][unsigned(TimingStage::Swap)];
                    require(swap.active, "Restored window must be exposed");
                    require(swap.count <= decode.count,
                            "New image count cannot exceed decoded image count");
                    require(s.latency[0][0].samples > 0 && s.latency[0][3].samples > 0,
                            "Both end-to-end timing paths available");
                    std::cout << s.json() << '\n';
                }
                phase++;
                app.quit();
            }
        });
        timer.start(250);
        QTimer::singleShot(30000, &app, &QCoreApplication::quit);
        app.exec();
        if (phase != 5)
            failed = 1;
        controller.shutdown();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
    return failed;
}
