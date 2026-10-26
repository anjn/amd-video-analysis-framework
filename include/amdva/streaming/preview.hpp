#pragma once
#include "amdva/gst_telemetry.hpp"
#include "amdva/runtime.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <gst/gst.h>
#include <gst/va/gstvadisplay.h>
#include <memory>
#include <mutex>
#include <string>
namespace amdva::streaming {
// Shared admission and persistent preview-surface bounds.
inline constexpr unsigned max_video_width = 2560;
inline constexpr unsigned max_video_height = 1440;
// Call before creating GStreamer, Qt or worker threads.
void initialize_runtime();
using Clock = std::chrono::steady_clock;
using Sample = std::shared_ptr<GstSample>;
// One BGRA preview image on a VA surface.
struct Plane {
    TraceToken timing;
    VASurfaceID surface{VA_INVALID_SURFACE};
    // 0 free; 1 complete; 2 displayed; 3 fence pending. GL objects belong
    // exclusively to the UI thread; VA writes only to state 0.
    int state{};
    unsigned width{}, height{}, original_width{}, original_height{};
    uint64_t generation{}, sequence{};
    std::string recording_channel, recording_session;
    int64_t recording_start_ms{}, recording_end_ms{};
    int64_t wall_time_ms{}; // Receiver-based recording datetime for this playback frame.
    Clock::time_point time;
};
// Triple-buffered GPU output of a decoder. A producer scales into a free plane and
// marks it complete; the consumer displays it and frees it. Lock `mutex` for access.
struct PreviewSlot {
    std::mutex mutex;
    std::unique_ptr<VaSession> va, preview_va;
    std::array<unsigned, 4> preview_geometry{};
    std::array<Plane, 3> preview;
    std::string channel;
    uint64_t generation{}, sequence{};
    int64_t playback_until_ms{}; // Exclusive playback bound; zero means unbounded.
    unsigned target_width{640}, target_height{360};
    PreviewSlot() = default;
    PreviewSlot(const PreviewSlot &) = delete;
    PreviewSlot &operator=(const PreviewSlot &) = delete;
    virtual ~PreviewSlot();
};
// Caller holds the slot mutex. Pixels remain on VA surfaces.
void prepare_preview(GstVaDisplay *, PreviewSlot &, Plane &, GstSample *, uint64_t generation,
                     uint64_t sequence);
} // namespace amdva::streaming
