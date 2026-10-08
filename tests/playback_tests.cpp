#include "amdva/streaming/playback.hpp"
#include <fstream>
#include <gst/va/gstvadisplay_drm.h>
#include <iostream>
#include <unistd.h>
using namespace amdva::streaming;
int main() {
    initialize_runtime();
    bool rejected = false;
    try {
        PlaybackSession missing(nullptr);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    if (!rejected) {
        std::cerr << "Null VA display accepted\n";
        return 1;
    }
    const auto root =
        std::filesystem::temp_directory_path() / ("amdva-playback-" + std::to_string(getpid()));
    auto *display = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    try {
        if (!display)
            throw std::runtime_error("Missing VA display");
        Catalog catalog(root);
        {
            PlaybackSession playback(display);
            for (unsigned i = 0; i < 3; i++) {
                const auto name = "bad" + std::to_string(i) + ".mkv";
                std::ofstream(root / name) << "not a Matroska recording";
                catalog.opened(name, "fixture");
                catalog.closed(name);
                playback.open(catalog.open_recording(name));
                auto deadline = Clock::now() + std::chrono::seconds(5);
                while (playback.status().state != "error" && Clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (playback.status().state != "error" || playback.status().error.empty())
                    throw std::runtime_error("Corrupt recording did not report isolated error");
                playback.stop();
                deadline = Clock::now() + std::chrono::seconds(2);
                while (playback.status().state != "stopped" && Clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (playback.status().state != "stopped")
                    throw std::runtime_error("Cannot stop failed playback");
                if (!catalog.enforce(1, 0))
                    throw std::runtime_error("Failed playback retained recording pin");
            }
        }
        gst_object_unref(display);
        std::filesystem::remove_all(root);
        std::cout << "Corrupt recording, stop/reopen and playback pin release passed\n";
        return 0;
    } catch (const std::exception &e) {
        if (display)
            gst_object_unref(display);
        std::filesystem::remove_all(root);
        std::cerr << e.what() << "\n";
        return 1;
    }
}
