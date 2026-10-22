#pragma once
#include "amdva/model.hpp"
#include <functional>
#include <future>
namespace amdva {
struct BatchOptions {
    std::string model, model_info, instance_id;
    unsigned size{1}, timeout_ms{20}, queue_limit{64};
    int device{};
    float confidence{.25f}, nms{.45f};
    bool raw_outputs{};
    // Completed stage timings per execution batch; callback must not throw.
    std::function<void(double, double, unsigned)> measured;
};
struct InferenceRequest {
    DeviceTensor tensor;
    std::string source;
    uint64_t generation{}, sequence{};
};
class SharedBatchExecutor {
  public:
    static std::shared_ptr<SharedBatchExecutor> acquire(const BatchOptions &);
    ~SharedBatchExecutor();
    std::future<AnalysisResult> submit(InferenceRequest);
    std::vector<std::future<AnalysisResult>> submit_many(std::vector<InferenceRequest>);
    // Retire a source generation; queued jobs fail and in-flight jobs cannot
    // deliver results into a replacement stream.
    void cancel(const std::string &source, uint64_t generation);
    // Final retirement: caller must never submit this source again.
    void release_source(const std::string &source, uint64_t generation);
    const ModelInfo &info() const;

  private:
    explicit SharedBatchExecutor(BatchOptions);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva
