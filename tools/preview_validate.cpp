#include "amdva/streaming/playback.hpp"
#include <algorithm>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <gst/va/gstvadisplay_drm.h>
#include <iostream>
using namespace amdva::streaming;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "Usage: amdva-preview-validate H264.mkv H265.mkv NEW_ROOT [GOP_MS]\n";
        return 2;
    }
    const int64_t gop_ms = argc == 5 ? std::stoll(argv[4]) : 1000;
    if (gop_ms <= 0)
        return 2;
    initialize_runtime();
    auto *display = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    try {
        check(display, "Missing display");
        check(!std::filesystem::exists(argv[3]), "Root must be new");
        Catalog catalog(argv[3]);
        const int64_t base = 100000;
        for (int i = 0; i < 100; i++) {
            auto name = std::to_string(i) + ".mkv";
            std::filesystem::copy_file(argv[1 + i % 2], std::filesystem::path(argv[3]) / name);
            catalog.opened(name, std::to_string(i));
            catalog.media_start(name, base);
            catalog.closed(name, 2 * GST_SECOND);
        }
        auto audit = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        if (audit)
            audit();
        // Exercise full-size preview output as well as the small hover thumbnails.
        // This catches mismatches between admission limits and allocated surfaces.
        for (const auto *name : {"0.mkv", "1.mkv"}) {
            PlaybackSession full(display);
            {
                std::lock_guard<std::mutex> lock(full.slot().mutex);
                full.slot().target_width = max_video_width;
                full.slot().target_height = max_video_height;
            }
            full.open(catalog.open_recording(name), false);
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (!full.status().frames && full.status().state != "error" &&
                   Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            check(full.status().frames > 0, "Full-size playback preview missing");
            std::lock_guard<std::mutex> lock(full.slot().mutex);
            const auto &planes = full.slot().preview;
            const auto ready = std::find_if(planes.begin(), planes.end(),
                                            [](const auto &p) { return p.state == 1; });
            check(ready != planes.end(), "Full-size preview plane missing");
            check(ready->width <= max_video_width && ready->height <= max_video_height &&
                      (ready->width == max_video_width || ready->height == max_video_height),
                  "Full-size preview did not reach display bounds");
            std::cout << "Full preview: source=" << ready->original_width << "x"
                      << ready->original_height << " output=" << ready->width << "x"
                      << ready->height << '\n';
        }
        KeyframePreviewSession preview(display, catalog);
        auto obtain = [&](std::string camera, int64_t time) {
            auto id = preview.request(camera, time);
            auto deadline = Clock::now() + std::chrono::seconds(3);
            KeyframePreviewStatus s;
            do {
                s = preview.status();
                if (s.request == id && s.state != "loading")
                    return s;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (Clock::now() < deadline);
            throw std::runtime_error("Preview timeout");
        };
        std::vector<double> times;
        for (int i = 0; i < 100; i++) {
            // Forward requests miss the proven keyframe interval even in an existing GOP.
            auto s = obtain(std::to_string(i), base + 200 + i * 16);
            if (!s.image)
                std::cerr << "request " << i << " state=" << s.state << '\n';
            check(s.image && s.state == "ready", "GPU preview missing");
            check(s.image->width <= 1280u && s.image->height <= 720u,
                  "Preview exceeds safe VPE bounds");
            check(s.image->frame_ms <= s.requested_ms && s.image->frame_ms >= base,
                  "Keyframe snapshot is after target");
            check(s.image->frame_ms >= base + ((s.requested_ms - base) / gop_ms) * gop_ms - 34,
                  "Preview did not seek to a nearby keyframe");
            times.push_back(s.elapsed_ms);
        }
        auto cached = obtain("99", base + 200 + 99 * 16);
        check(cached.cache_hit, "Cache did not hit");
        check(preview.status().cached_images <= 8, "Cache unbounded");
        auto gap = obtain("0", base + 5000);
        check(gap.state == "missing" && !gap.image, "Gap preview invented");
        std::ofstream(std::filesystem::path(argv[3]) / "corrupt.mkv") << "invalid container";
        catalog.opened("corrupt.mkv", "broken");
        catalog.media_start("corrupt.mkv", base);
        catalog.closed("corrupt.mkv", 2 * GST_SECOND);
        auto bad = obtain("broken", base + 500);
        check(bad.state == "unavailable" && !bad.image && !bad.error.empty(),
              "Corrupt thumbnail did not fail with a diagnostic");
        for (int i = 0; i < 50; i++)
            preview.request(std::to_string(i % 2), base + 100 + i);
        preview.cancel().get();
        check(!preview.status().image, "Cancelled result escaped");
        auto s = obtain("0", base + 1500);
        check(bool(s.image), "Preview after cancellation missing");
        auto before = catalog.deletion_generation();
        auto job = catalog.begin_delete();
        while (!job.done())
            catalog.delete_batch(job);
        check(catalog.deletion_generation() > before && !preview.status().image,
              "Deleted cached image remained visible");
        check(job.protected_files == 0, "Thumbnail retained input pin");
        std::sort(times.begin(), times.end());
        if (errors)
            check(errors() == 0, "GPU copy audit failed");
        std::cout << "{\"samples\":100,\"p50_ms\":" << times[49] << ",\"p95_ms\":" << times[94]
                  << ",\"max_ms\":" << times.back() << ",\"cached_ms\":" << cached.elapsed_ms
                  << ",\"audit_errors\":" << (errors ? errors() : 0) << "}\n";
        gst_object_unref(display);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        if (display)
            gst_object_unref(display);
        return 1;
    }
}
