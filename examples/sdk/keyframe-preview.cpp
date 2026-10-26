#include <amdva/streaming/playback.hpp>
#include <gst/va/gstvadisplay_drm.h>
#include <iostream>
#include <unistd.h>
#include <thread>
using namespace amdva::streaming;
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: keyframe-preview RECORDING_ROOT CAMERA_ID UNIX_MS\n";
        return 1;
    }
    initialize_runtime();
    auto *display = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    try {
        // Exclusive standalone catalog owner; never reuse an active NVR's root.
        Catalog catalog(argv[1]);
        KeyframePreviewSession preview(display, catalog);
        const auto id = preview.request(argv[2], std::stoll(argv[3]));
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        bool ready = false;
        while (Clock::now() < deadline) {
            auto state = preview.status();
            if (state.request == id && state.state != "loading") {
                if (!state.image)
                    throw std::runtime_error(state.error.empty() ? state.state : state.error);
                // Import these DMA-BUFs into a GPU renderer; do not map their pixels.
                // A renderer retains state.image until its sampling fence signals.
                auto descriptor = state.image->export_surface();
                for (unsigned i = 0; i < descriptor.num_objects; ++i)
                    ::close(descriptor.objects[i].fd);
                std::cout << state.image->width << 'x' << state.image->height << ", frame "
                          << state.image->frame_ms << " ms, " << state.elapsed_ms << " ms\n";
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        preview.cancel().get();
        if (!ready)
            throw std::runtime_error("Preview request timed out");
        gst_object_unref(display);
        return 0;
    } catch (const std::exception &e) {
        if (display)
            gst_object_unref(display);
        std::cerr << e.what() << '\n';
        return 2;
    }
}
