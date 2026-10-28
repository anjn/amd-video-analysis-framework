#include "window.hpp"
#include <QApplication>
#include <QSurfaceFormat>
#include <iostream>
int main(int argc, char **argv) {
    const bool fullscreen = argc == 3 && std::string(argv[1]) == "--fullscreen";
    if (argc != 2 && !fullscreen) {
        std::cerr << "Usage: amdva-nvr [--fullscreen] CONFIG.json\n";
        return 1;
    }
    const std::string config = argv[argc - 1];
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
        // Clear the previous run before any camera starts recording. Catalog owns
        // the exclusive root lock and removes dependent search/analysis records.
        {
            amdva::streaming::Catalog catalog(options.recording_root);
            auto deletion = catalog.begin_delete();
            while (!deletion.done())
                catalog.delete_batch(deletion);
            if (deletion.failed || deletion.protected_files)
                throw std::runtime_error("Cannot clear previous recordings at startup");
            std::cout << "Cleared previous recordings: " << deletion.deleted << std::endl;
        }
        amdva::nvr::Controller controller(options);
        amdva::nvr::Window window(controller, options, config);
        if (fullscreen)
            window.showFullScreen();
        else
            window.show();
        return app.exec();
    } catch (const std::exception &e) {
        std::cerr << "NVR initialization failed: " << e.what() << "\n";
        return 2;
    }
}
