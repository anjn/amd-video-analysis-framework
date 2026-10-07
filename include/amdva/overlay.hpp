#pragma once
#include "amdva/tracking.hpp"
namespace amdva {
struct OverlayObject {
    Detection box{};
    std::string label;
    uint64_t tracking_id{};
    std::vector<Keypoint> keypoints;
};
struct OverlayFrame {
    unsigned width{}, height{};
    std::vector<OverlayObject> objects;
    std::vector<AnalysisAttachment> attachments;
    std::vector<std::string> labels;
};
struct OverlayLine {
    int x0{}, y0{}, x1{}, y1{};
    uint32_t bgra{0xff33ff1a};
};
OverlayFrame overlay_from_buffer(GstBuffer *, unsigned width, unsigned height);
std::vector<OverlayLine> make_overlay_lines(const OverlayFrame &, unsigned target_width,
                                            unsigned target_height);
class OverlayRenderer {
  public:
    explicit OverlayRenderer(int device = 0);
    ~OverlayRenderer();
    void draw(ImportedSurface &, unsigned width, unsigned height, const OverlayFrame &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva
