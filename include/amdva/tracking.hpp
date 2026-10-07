#pragma once
#include "amdva/tensor_meta.hpp"
namespace amdva {
struct TrackedObject {
    Detection box{};
    uint64_t id{};
    std::string label;
};
// One instance per continuous stream. Does not map or copy image pixels.
class MetadataTracker {
  public:
    explicit MetadataTracker(ModelInfo info, const std::string &mode = "short-term-imageless");
    ~MetadataTracker();
    std::vector<TrackedObject> process(GstSample *, const std::vector<Detection> *observations,
                                       const FrameIdentity &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva
