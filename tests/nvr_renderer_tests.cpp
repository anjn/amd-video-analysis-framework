#include "window.hpp"
#include <QApplication>
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
        QTimer timer;
        QObject::connect(&timer, &QTimer::timeout, [&] { tiles.update(); });
        timer.start(33);
        QTimer::singleShot(1000, [&] { tiles.resize(64, 64); });
        QTimer::singleShot(2000, [&] { tiles.resize(640, 360); });
        QTimer::singleShot(3000, &app, &QCoreApplication::quit);
        app.exec();
        error = tiles.error();
        controller.shutdown();
    }
    std::filesystem::remove_all(root);
    if (!error.empty()) {
        std::cerr << error << "\n";
        return 1;
    }
    std::cout << "Persistent VA/HIP/EGL pool resize and cleanup passed\n";
}
