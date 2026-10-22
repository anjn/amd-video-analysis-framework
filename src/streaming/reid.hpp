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
    std::vector<ReIdFeature> encode_many(const std::vector<SearchImage> &);
    double prepare_ms{}, inference_ms{};
    const std::vector<size_t> &input_shape() const { return input_.shape; }
};
} // namespace amdva::streaming
