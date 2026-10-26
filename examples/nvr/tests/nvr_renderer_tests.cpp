#include "window.hpp"
#include <QApplication>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QTimer>
#include <iostream>
#include <unistd.h>
int main(int argc, char **argv) {
    amdva::nvr::initialize_runtime();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    std::string error;
    const auto root =
        std::filesystem::temp_directory_path() / ("amdva-render-" + std::to_string(getpid()));
    {
        amdva::nvr::Options options;
        options.model = "/missing";
        options.single_model = "/missing";
        options.model_info = "/missing";
        if (argc == 2) {
            auto loaded = amdva::nvr::load_config(argv[1]);
            options.model = loaded.model;
            options.single_model = loaded.single_model;
            options.model_info = loaded.model_info;
        }
        options.recording_root = root;
        amdva::nvr::Controller controller(options);
        amdva::nvr::Tiles tiles(controller);
        tiles.resize(640, 360);
        tiles.show();
        std::array<std::string, 16> cells{};
        tiles.layout(cells, 4);
        unsigned selected = 99, clicks = 0;
        tiles.activated = [&](unsigned index, bool twice) {
            if (!twice) { selected = index; ++clicks; }
        };
        const QPointF point(480, 90);
        QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&tiles, &press);
        if (selected != 1 || clicks != 1)
            error = "Grid selection did not activate on mouse press";
        QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&tiles, &release);
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] { tiles.update(); });
        timer.start(33);
        QTimer::singleShot(1000, [&] { tiles.resize(64, 64); });
        QTimer::singleShot(2000, [&] { tiles.resize(640, 360); });
        QTimer::singleShot(3000, &app, &QCoreApplication::quit);
        app.exec();
        if (!tiles.error().empty())
            error = tiles.error();
        if (clicks != 1)
            error = "Grid release scheduled a duplicate activation";
        controller.shutdown();
    }
    std::filesystem::remove_all(root);
    if (!error.empty()) {
        std::cerr << error << "\n";
        return 1;
    }
    std::cout << "Persistent VA/HIP/EGL pool resize and cleanup passed\n";
}
