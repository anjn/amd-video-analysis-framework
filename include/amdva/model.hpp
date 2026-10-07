#pragma once
#include "amdva/inference.hpp"
#include "amdva/preprocess.hpp"
#include <map>
#include <mutex>
namespace amdva {
enum class TensorType { Float32, Float16, Int32, Int64, UInt8 };
struct DeviceTensor {
    std::string name;
    TensorType type{TensorType::Float32};
    std::vector<size_t> shape;
    std::shared_ptr<DeviceBuffer> storage;
    int device{};
    size_t elements() const;
};
struct ModelInfo {
    std::string task{"detection"}, adapter{"yolov8"}, input_name{"images"};
    unsigned width{640}, height{640}, classes{80}, top_k{5}, keypoints{17};
    bool letterbox{true};
    PixelNormalization normalization;
    std::vector<std::string> labels;
    static ModelInfo load(const std::string &path);
};
// Named static-shape GPU I/O. Outputs own their storage independently of the
// executor and remain valid across subsequent evaluations and model teardown.
class ModelExecutor {
  public:
    explicit ModelExecutor(const std::string &model, int device = 0);
    ~ModelExecutor();
    std::vector<DeviceTensor> inputs() const;
    std::vector<DeviceTensor> outputs() const;
    std::vector<DeviceTensor> infer(const std::map<std::string, DeviceTensor> &inputs);
    unsigned batch() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct Classification {
    int class_id{};
    float score{};
};
struct Keypoint {
    float x{}, y{}, confidence{};
};
struct Instance {
    Detection box{};
    std::vector<Classification> classifications;
    std::vector<Keypoint> keypoints;
    int mask_index{-1};
    uint64_t tracking_id{};
};
struct AnalysisResult {
    unsigned batch_index{};
    std::vector<Instance> objects;
    std::vector<Classification> classifications;
    // Instance masks [N,H,W] float32 probabilities, semantic mask [H,W] int32.
    std::vector<DeviceTensor> tensors;
};
class ModelPostprocessor {
  public:
    explicit ModelPostprocessor(ModelInfo info, int device = 0);
    ~ModelPostprocessor();
    std::vector<AnalysisResult> process(const std::vector<DeviceTensor> &outputs, unsigned valid,
                                        float confidence = .25f, float nms = .45f);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva
