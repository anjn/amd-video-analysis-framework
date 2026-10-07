#pragma once
#include "amdva/runtime.hpp"
#include <array>
namespace amdva {
struct ImageTransform {
    unsigned original_width{}, original_height{}, width{}, height{};
    unsigned scaled_width{}, scaled_height{}, pad_x{}, pad_y{};
    unsigned crop_x{}, crop_y{}, crop_width{}, crop_height{};
    static ImageTransform make(unsigned source_width, unsigned source_height, unsigned width,
                               unsigned height, bool letterbox = true, unsigned x = 0,
                               unsigned y = 0, unsigned w = 0, unsigned h = 0);
    std::array<float, 4> to_source(float x, float y, float w, float h) const;
};
struct PixelNormalization {
    bool bgr{};
    std::array<float, 3> mean{0, 0, 0}, stddev{1, 1, 1};
    float scale{1.0f / 255}, padding{114};
};
// Packs a persistently imported VPE BGRA surface into device float32 NCHW.
// The caller owns the VA/HIP lifetime and waits before overwriting the source.
class ImagePacker {
  public:
    explicit ImagePacker(int device = 0);
    void pack(const ImportedSurface &, const ImageTransform &, void *destination,
              const PixelNormalization & = {}, hipStream_t stream = nullptr);
    void pad(void *destination, unsigned valid, unsigned batch, unsigned width, unsigned height,
             const PixelNormalization & = {}, hipStream_t stream = nullptr);

  private:
    int device_{};
    Kernel pack_, pad_;
};
} // namespace amdva
