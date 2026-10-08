#include <amdva/streaming/playback.hpp>
#include <gst/va/gstvadisplay_drm.h>
#include <iostream>
using namespace amdva::streaming;
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: playback RECORDING_ROOT RECORDING_NAME\n";
        return 1;
    }
    initialize_runtime();
    auto *display = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    try {
        Catalog catalog(argv[1]); // Standalone owner; do not open a root used by another writer.
        {
            PlaybackSession playback(display);
            playback.open(catalog.open_recording(argv[2]));
            auto deadline = Clock::now() + std::chrono::seconds(15);
            while (Clock::now() < deadline) {
                auto status = playback.status();
                if (status.state == "error")
                    throw std::runtime_error(status.error);
                // A headless consumer releases completed previews. A GUI instead imports
                // these VA surfaces into EGL and releases them after its GL fence.
                {
                    auto &slot = playback.slot();
                    std::lock_guard<std::mutex> lock(slot.mutex);
                    for (auto &plane : slot.preview)
                        if (plane.state == 1)
                            plane.state = 0;
                }
                if (status.state == "ended") {
                    std::cout << status.frames << " frames, " << status.duration_ns << " ns\n";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (playback.status().frames == 0)
                throw std::runtime_error("No decoded playback frames");
        }
        gst_object_unref(display);
        return 0;
    } catch (const std::exception &e) {
        if (display)
            gst_object_unref(display);
        std::cerr << e.what() << "\n";
        return 2;
    }
}
