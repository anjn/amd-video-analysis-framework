#include "amdva/model.hpp"
namespace amdva {
struct BatchEngine::Impl {
    ModelInfo info;
    ModelExecutor engine;
    ModelPostprocessor post;
    unsigned batch;
    Impl(const std::string &model, const std::string &info_path, unsigned b, int device)
        : info(ModelInfo::load(info_path)), engine(model, device), post(info, device), batch(b) {
        if (info.task != "detection" || info.adapter != "yolov8" || info.width != 640 ||
            info.height != 640 || info.classes != 80)
            throw std::runtime_error("BatchEngine compatibility API requires YOLOv8 COCO; use "
                                     "ModelExecutor for other models");
        const auto inputs = engine.inputs();
        if (engine.batch() != batch || inputs.size() != 1 || inputs[0].name != info.input_name ||
            inputs[0].type != TensorType::Float32 ||
            inputs[0].shape != std::vector<size_t>{batch, 3, 640, 640})
            throw std::runtime_error("Incompatible YOLO batch input");
        const auto outputs = engine.outputs();
        if (outputs.size() != 1 || outputs[0].shape != std::vector<size_t>{batch, 84, 8400})
            throw std::runtime_error("Incompatible YOLO output");
    }
};
BatchEngine::BatchEngine(const std::string &model, const std::string &info, unsigned batch,
                         int device)
    : device_(device) {
    if (batch != 1 && batch != 2 && batch != 4 && batch != 8 && batch != 16)
        throw std::runtime_error("Batch must be 1,2,4,8 or 16");
    hip_check(hipSetDevice(device), "Batch device");
    impl_ = std::make_unique<Impl>(model, info, batch, device);
}
BatchEngine::~BatchEngine() {
    if (impl_) {
        (void)hipSetDevice(device_);
        (void)hipDeviceSynchronize();
    }
}
const std::vector<std::string> &BatchEngine::labels() const { return impl_->info.labels; }
std::vector<std::vector<Detection>> BatchEngine::infer(void *input, unsigned valid,
                                                       float confidence, float nms) {
    auto &s = *impl_;
    if (!input || !valid || valid > s.batch)
        throw std::runtime_error("Invalid batch occupancy");
    hip_check(hipSetDevice(device_), "Batch device");
    auto tensor = s.engine.inputs()[0];
    tensor.storage =
        std::make_shared<DeviceBuffer>(input, size_t(s.batch) * 3 * 640 * 640 * sizeof(float));
    auto outputs = s.engine.infer({{tensor.name, tensor}});
    auto analyzed = s.post.process(outputs, valid, confidence, nms);
    std::vector<std::vector<Detection>> result(valid);
    for (unsigned i = 0; i < valid; i++)
        for (const auto &object : analyzed[i].objects)
            result[i].push_back(object.box);
    return result;
}
} // namespace amdva
