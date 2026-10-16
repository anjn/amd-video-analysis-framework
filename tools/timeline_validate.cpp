#include "amdva/streaming/playback.hpp"
#include <dlfcn.h>
#include <fstream>
#include <gst/va/gstvadisplay_drm.h>
#include <iostream>
using namespace amdva::streaming;
namespace fs = std::filesystem;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: amdva-timeline-validate H264.mkv H265.mkv NEW_RECORDING_ROOT\n";
        return 2;
    }
    initialize_runtime();
    auto *display = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    try {
        require(display, "Missing VA display");
        require(!fs::exists(argv[3]), "Recording root must be new");
        Catalog catalog(argv[3]);
        const int64_t base = g_get_real_time() / 1000 - 60000;
        auto add = [&](std::string name, int64_t offset, bool h265 = false) {
            fs::copy_file(argv[h265 ? 2 : 1], fs::path(argv[3]) / name);
            catalog.opened(name, "camera", "Camera", name, 0);
            catalog.media_start(name, base + offset);
            catalog.closed(name, 2 * GST_SECOND);
        };
        for (int i = 0; i < 6; i++)
            add("part-" + std::to_string(i) + ".mkv", i * 2000);
        add("gap-h265.mkv", 15000, true);
        std::ofstream(fs::path(argv[3]) / "corrupt.mkv") << "not a video";
        catalog.opened("corrupt.mkv", "camera");
        catalog.media_start("corrupt.mkv", base + 17000);
        catalog.closed("corrupt.mkv", 2 * GST_SECOND);
        add("after-corrupt.mkv", 19000);
        // Legacy metadata indexing reads the container without a decoder.
        fs::copy_file(argv[1], fs::path(argv[3]) / "legacy.mkv");
        catalog.opened("legacy.mkv", "legacy");
        catalog.closed("legacy.mkv");
        catalog.media_start("legacy.mkv", base);
        catalog.index_unknown("legacy");
        require(catalog.coverage("legacy").pending_durations == 0,
                "Legacy duration indexing failed");
        auto audit_start = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        auto audit_errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        if (audit_start)
            audit_start();
        TimelinePlaybackSession player(display, catalog);
        player.open("camera", base, true, true);
        auto start = Clock::now(), last_frame = start;
        double max_gap = 0;
        bool paused = false, sought = false, resumed = false, appended = false, followed = false,
             switched = false;
        uint64_t before_pause = 0, last_seq = 0;
        auto paused_at = start, wait_at = start;
        std::string previous_state;
        while (Clock::now() - start < std::chrono::seconds(40)) {
            auto now = Clock::now();
            auto state = player.status();
            require(state.state != "error", state.error.c_str());
            if (state.state != previous_state) {
                std::cout << "state=" << state.state << " time=" << state.position_ms - base
                          << " frames=" << state.frames << " transitions=" << state.transitions
                          << " skipped=" << state.skipped << std::endl;
                previous_state = state.state;
            }
            {
                auto &slot = player.slot();
                std::lock_guard<std::mutex> lock(slot.mutex);
                for (auto &p : slot.preview)
                    if (p.state == 1) {
                        if (p.generation == slot.generation && p.sequence > last_seq) {
                            if (last_seq && (!paused || resumed) && state.state == "playing")
                                max_gap = std::max(
                                    max_gap,
                                    std::chrono::duration<double, std::milli>(now - last_frame)
                                        .count());
                            last_seq = p.sequence;
                            last_frame = now;
                        }
                        p.state = 0;
                    }
            }
            if (state.frames > 20 && !paused) {
                player.pause();
                paused = true;
                paused_at = now;
                before_pause = state.frames;
            }
            if (paused && !sought && now - paused_at > std::chrono::milliseconds(150)) {
                require(state.frames <= before_pause + 2, "Pause did not settle");
                player.seek(base + 3500);
                sought = true;
                paused_at = now;
            }
            if (sought && !resumed && now - paused_at > std::chrono::milliseconds(300)) {
                require(state.state == "paused" && state.frames > 0,
                        "Paused seek did not produce preroll");
                require(std::abs(state.position_ms - (base + 3500)) < 100,
                        "Paused seek position wrong");
                std::cout << "Paused seek position=" << state.position_ms - base << "ms\n";
                player.play();
                resumed = true;
                last_frame = now;
                last_seq = 0;
            }
            if (resumed && state.state == "waiting" && !appended) {
                require(state.transitions >= 5 && state.skipped == 1,
                        "Continuation, gap or corrupt-file skip failed");
                require(max_gap < 250, "Boundary frame interval exceeded 250ms");
                std::cout << "Continuous frames=" << state.frames
                          << " transitions=" << state.transitions << " skipped=" << state.skipped
                          << " max_frame_gap_ms=" << max_gap << "\n";
                add("appended.mkv", 21000, true);
                appended = true;
                wait_at = now;
            }
            if (appended && state.position_ms >= base + 21000 && state.state == "playing")
                followed = true;
            if (followed && state.state == "waiting" && !switched) {
                player.pause();
                player.open("legacy", base + 500, false, false);
                switched = true;
                wait_at = now;
            }
            if (switched && now - wait_at > std::chrono::milliseconds(500)) {
                require(state.channel == "legacy" && state.state == "paused" && state.frames > 0,
                        "Camera switch failed");
                // Last seek wins; no old-camera frame may escape its generation.
                player.open("camera", base + 1000, false, false);
                player.open("legacy", base + 750, false, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                {
                    auto &slot = player.slot();
                    std::lock_guard<std::mutex> lock(slot.mutex);
                    for (auto &p : slot.preview)
                        if (p.state == 1)
                            p.state = 0;
                }
                auto final = player.status();
                require(final.channel == "legacy" && final.state == "paused",
                        "Rapid seek/camera switch failed");
                player.stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                require(catalog.enforce(1, 0), "Playback or lookahead pin retained after stop");
                if (audit_errors)
                    require(audit_errors() == 0, "Copy audit failed");
                std::cout << "audit_errors=" << (audit_errors ? audit_errors() : 0) << "\n";
                std::cout
                    << "Latest-fragment follow, H265, camera changes and pin release passed\n";
                gst_object_unref(display);
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        auto state = player.status();
        std::cerr << "Timeout state=" << state.state << " frames=" << state.frames
                  << " position=" << state.position_ms - base
                  << " transitions=" << state.transitions << " notice=" << state.notice << "\n";
        throw std::runtime_error("Timeline validation timed out");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        if (display)
            gst_object_unref(display);
        return 1;
    }
}
