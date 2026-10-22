#pragma once
#include "amdva/inference.hpp"
#include <array>
namespace amdva::streaming {
using ReIdFeature = std::array<float, 512>;
struct PersonDetection {
    Detection box{};
    ReIdFeature feature{};
};
struct PersonIdentity {
    uint64_t track{}, person{};
    bool confirmed{};
};
// Per-camera, timestamp-based BoT-SORT-style association for fixed cameras (no GMC).
// A separate conservative gallery reconnects tracklets after short absences.
class PersonTracker {
  public:
    PersonTracker();
    ~PersonTracker();
    std::vector<PersonIdentity> update(int64_t time_ms, const std::vector<PersonDetection> &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
float reid_similarity(const ReIdFeature &, const ReIdFeature &);
} // namespace amdva::streaming
