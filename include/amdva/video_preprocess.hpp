#pragma once
#include "amdva/tensor_meta.hpp"
#include <gst/video/video.h>
namespace amdva {
class VideoPreprocessor {
  public:
    explicit VideoPreprocessor(ModelInfo info, int device = 0);
    ~VideoPreprocessor();
    std::shared_ptr<Tensor> prepare(GstBuffer *, const GstVideoInfo &, unsigned x = 0,
                                    unsigned y = 0, unsigned width = 0, unsigned height = 0);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva
