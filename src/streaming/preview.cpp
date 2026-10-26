#include "amdva/streaming/preview.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <malloc.h>
#include <stdexcept>
#include <gst/va/gstva.h>
#include <gst/video/video.h>
namespace amdva::streaming {
void initialize_runtime() {
    // Reuse a bounded set of glibc arenas across transient camera worker groups.
    // Respect an operator's explicit allocator setting.
    const auto *arenas = std::getenv("MALLOC_ARENA_MAX");
    if (!arenas || !*arenas)
        mallopt(M_ARENA_MAX, 4);
    gst_init(nullptr, nullptr);
}
PreviewSlot::~PreviewSlot() {
    preview_va.reset();
    if (va)
        for (auto &p : preview)
            va->destroy_surface(p.surface);
}
void prepare_preview(GstVaDisplay *display, PreviewSlot &s, Plane &out, GstSample *sample,
                     uint64_t generation, uint64_t sequence) {
    GstVideoInfo info{};
    auto *buffer = gst_sample_get_buffer(sample);
    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 || info.width <= 0 ||
        info.height <= 0 || info.width > int(max_video_width) ||
        info.height > int(max_video_height) || info.width % 2 || info.height % 2 ||
        info.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
        (info.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN &&
         info.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709) ||
        (info.colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN &&
         info.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235))
        throw std::runtime_error("Unsupported preview format (progressive NV12 BT.709 limited, up "
                                 "to 2560x1440 required)");
    auto *crop = gst_buffer_get_video_crop_meta(buffer);
    if (crop && (crop->x || crop->y || crop->width != unsigned(info.width) ||
                 crop->height != unsigned(info.height)))
        throw std::runtime_error("Cropped preview surfaces are unsupported");
    if (gst_va_buffer_peek_display(buffer) != display)
        throw std::runtime_error("Decoder did not use shared VA display");
    auto surface = gst_va_buffer_get_surface(buffer);
    if (surface == VA_INVALID_SURFACE)
        throw std::runtime_error("Missing VA decode surface");
    double factor =
        std::clamp(std::min(double(std::min(max_video_width, s.target_width)) / info.width,
                            double(std::min(max_video_height, s.target_height)) / info.height),
                   1.0 / 6.0, 16.0);
    out.width = std::max(2u, 2 * unsigned(std::ceil(info.width * factor / 2)));
    out.height = std::max(2u, 2 * unsigned(std::ceil(info.height * factor / 2)));
    std::array<unsigned, 4> geometry{unsigned(info.width), unsigned(info.height), out.width,
                                     out.height};
    if (!s.preview_va || s.preview_geometry != geometry) {
        s.preview_va.reset();
        s.preview_va =
            std::make_unique<VaSession>(static_cast<VADisplay>(gst_va_display_get_va_dpy(display)));
        s.preview_geometry = geometry;
    }
    s.preview_va->scale(surface, out.surface, info.width, info.height, out.width, out.height);
    out.original_width = info.width;
    out.original_height = info.height;
    out.wall_time_ms = 0;
    out.generation = generation;
    out.sequence = sequence;
}
} // namespace amdva::streaming
