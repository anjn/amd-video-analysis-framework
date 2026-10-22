#pragma once
#include "amdva/streaming/person_tracking.hpp"
#include "amdva/streaming/search.hpp"
namespace amdva::streaming {
class ReIdModel {
    ModelExecutor model_;
    DeviceTensor input_;

  public:
    explicit ReIdModel(const std::string &path, unsigned device);
    ReIdFeature encode(const SearchImage &);
};
} // namespace amdva::streaming
