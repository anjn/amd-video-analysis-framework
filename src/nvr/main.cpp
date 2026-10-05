#include "window.hpp"
#include <QApplication>
#include <QSurfaceFormat>
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: amdva-nvr CONFIG.json\n";
        return 1;
    }
    const std::string config = argv[1];
    amdva::nvr::initialize_runtime();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    amdva::nvr::install_shutdown_signals(app);
    try {
        auto options = amdva::nvr::load_config(config);
        amdva::nvr::Controller controller(options);
        amdva::nvr::Window window(controller, options, config);
        window.show();
        return app.exec();
    } catch (const std::exception &e) {
        std::cerr << "NVR initialization failed: " << e.what() << "\n";
        return 2;
    }
}
