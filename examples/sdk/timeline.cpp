#include <amdva/streaming/playback.hpp>
#include <gst/va/gstvadisplay_drm.h>
#include <iostream>
using namespace amdva::streaming;
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: timeline RECORDING_ROOT CAMERA_ID\n";
        return 1;
    }
    initialize_runtime();
    auto *display = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    try {
        // Exclusive standalone catalog owner; never reuse an active NVR's root.
        Catalog catalog(argv[1]);
        auto coverage = catalog.coverage(argv[2]);
        while (coverage.pending_durations) {
            const auto previous = coverage.pending_durations;
            catalog.index_unknown(argv[2]);
            coverage = catalog.coverage(argv[2]);
            if (coverage.pending_durations >= previous)
                break; // A failed probe must not make this example wait forever.
        }
        if (coverage.intervals.empty())
            throw std::runtime_error("No known playable coverage for this camera");
        {
            TimelinePlaybackSession player(display, catalog);
            player.open(argv[2], coverage.intervals.front().from_ms, true, false);
            auto deadline = Clock::now() + std::chrono::seconds(60);
            while (Clock::now() < deadline) {
                auto state = player.status();
                if (state.state == "error")
                    throw std::runtime_error(state.error);
                // A headless consumer releases completed previews. GUI consumers
                // release surfaces only after the corresponding GL fence signals.
                {
                    auto &slot = player.slot();
                    std::lock_guard<std::mutex> lock(slot.mutex);
                    for (auto &plane : slot.preview)
                        if (plane.state == 1)
                            plane.state = 0;
                }
                if (state.state == "ended") {
                    std::cout << state.frames << " frames, " << state.transitions
                              << " transitions, position " << state.position_ms << " ms\n";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!player.status().frames)
                throw std::runtime_error("No decoded timeline frames");
        }
        gst_object_unref(display);
        return 0;
    } catch (const std::exception &e) {
        if (display)
            gst_object_unref(display);
        std::cerr << e.what() << '\n';
        return 2;
    }
}
