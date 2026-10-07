#include "amdva/model.hpp"
#include "model_json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <migraphx/migraphx.hpp>
namespace {
unsigned integer(JsonObject *o, const char *key, unsigned fallback, unsigned max = 4096) {
    if (!json_object_has_member(o, key))
        return fallback;
    auto *n = json_object_get_member(o, key);
    if (json_node_get_value_type(n) != G_TYPE_INT64)
        throw std::runtime_error(std::string("Expected integer: ") + key);
    auto v = json_node_get_int(n);
    if (v < 1 || v > max)
        throw std::runtime_error(std::string("Out of range: ") + key);
    return unsigned(v);
}
bool boolean(JsonObject *o, const char *key, bool fallback) {
    if (!json_object_has_member(o, key))
        return fallback;
    if (json_node_get_value_type(json_object_get_member(o, key)) != G_TYPE_BOOLEAN)
        throw std::runtime_error(std::string("Expected boolean: ") + key);
    return json_object_get_boolean_member(o, key);
}
std::string text(JsonObject *o, const char *key, const char *fallback) {
    return json_object_has_member(o, key) ? required_string(o, key) : fallback;
}
amdva::TensorType tensor_type(migraphx_shape_datatype_t t) {
    switch (t) {
    case migraphx_shape_float_type:
        return amdva::TensorType::Float32;
    case migraphx_shape_half_type:
        return amdva::TensorType::Float16;
    case migraphx_shape_int32_type:
        return amdva::TensorType::Int32;
    case migraphx_shape_int64_type:
        return amdva::TensorType::Int64;
    case migraphx_shape_uint8_type:
        return amdva::TensorType::UInt8;
    default:
        throw std::runtime_error("Unsupported tensor data type");
    }
}
void device_pointer(void *p, int device) {
    hipPointerAttribute_t a{};
    amdva::hip_check(hipPointerGetAttributes(&a, p), "Model tensor location");
    if (a.type != hipMemoryTypeDevice || a.device != device)
        throw std::runtime_error("Model tensor is not on the selected GPU");
}
amdva::DeviceTensor descriptor(const std::string &name, const migraphx::shape &s, int device) {
    // Kernels and downstream consumers require dense row-major storage.
    const auto dims = s.lengths(), strides = s.strides();
    size_t stride = 1;
    for (size_t i = dims.size(); i-- > 0;) {
        if (dims[i] > 1 && strides[i] != stride)
            throw std::runtime_error("Non-contiguous model tensor");
        stride *= dims[i];
    }
    return {name, tensor_type(s.type()), dims, {}, device};
}
} // namespace
namespace amdva {
size_t DeviceTensor::elements() const {
    size_t n = 1;
    for (auto d : shape) {
        if (!d || n > std::numeric_limits<size_t>::max() / d)
            throw std::runtime_error("Invalid tensor shape");
        n *= d;
    }
    return n;
}
ModelInfo ModelInfo::load(const std::string &path) {
    Parser p(path);
    auto *root = p.object();
    ModelInfo m;
    m.task = text(root, "task_type", "detection");
    m.adapter =
        text(root, "adapter",
             text(root, "family",
                  (m.task == "detection" || m.task == "instance-segmentation" || m.task == "pose")
                      ? "yolov8"
                      : m.task.c_str())
                 .c_str());
    auto *in = required_object(root, "input");
    auto *shape = required_array(in, "shape");
    for (unsigned i = 0; i < json_array_get_length(shape); i++)
        if (json_node_get_value_type(json_array_get_element(shape, i)) != G_TYPE_INT64)
            throw std::runtime_error("Input dimensions must be integers");
    if (json_array_get_length(shape) != 4 || json_array_get_int_element(shape, 0) < 1 ||
        json_array_get_int_element(shape, 0) > 16)
        throw std::runtime_error("Invalid input batch/rank");
    auto number = [](JsonNode *n) {
        auto type = json_node_get_value_type(n);
        if (type != G_TYPE_DOUBLE && type != G_TYPE_INT64)
            throw std::runtime_error("Expected model numeric value");
        auto value = json_node_get_double(n);
        if (!std::isfinite(value))
            throw std::runtime_error("Non-finite model number");
        return value;
    };
    if (json_array_get_length(shape) != 4 || json_array_get_int_element(shape, 1) != 3 ||
        text(in, "format", "NCHW") != "NCHW")
        throw std::runtime_error("Image input must be NCHW with three channels");
    auto h = json_array_get_int_element(shape, 2), w = json_array_get_int_element(shape, 3);
    if (w < 2 || w > 4096 || h < 2 || h > 4096)
        throw std::runtime_error("Invalid model image dimensions");
    m.width = unsigned(w);
    m.height = unsigned(h);
    m.input_name = text(in, "name", "images");
    m.letterbox =
        boolean(in, "letterbox", m.task != "classification" && m.task != "semantic-segmentation");
    auto order = text(in, "channel_order", "RGB");
    if (order != "RGB" && order != "BGR")
        throw std::runtime_error("Expected RGB or BGR");
    m.normalization.bgr = order == "BGR";
    if (json_object_has_member(in, "normalize")) {
        auto *a = required_array(in, "normalize");
        if (json_array_get_length(a) != 2 || number(json_array_get_element(a, 0)) != 0 ||
            number(json_array_get_element(a, 1)) != 1)
            throw std::runtime_error(
                "normalize must be [0,1]; use scale/mean/std for other conventions");
    }
    if (json_object_has_member(in, "scale"))
        m.normalization.scale = float(number(json_object_get_member(in, "scale")));
    if (json_object_has_member(in, "padding"))
        m.normalization.padding = float(number(json_object_get_member(in, "padding")));
    for (const auto key : {"mean", "std"})
        if (json_object_has_member(in, key)) {
            auto *a = required_array(in, key);
            if (json_array_get_length(a) != 3)
                throw std::runtime_error("mean/std require three values");
            for (unsigned i = 0; i < 3; i++)
                (std::string(key) == "mean" ? m.normalization.mean : m.normalization.stddev)[i] =
                    float(number(json_array_get_element(a, i)));
        }
    if (!std::isfinite(m.normalization.scale) || !std::isfinite(m.normalization.padding))
        throw std::runtime_error("Non-finite normalization");
    for (unsigned i = 0; i < 3; i++)
        if (!std::isfinite(m.normalization.mean[i]) || !std::isfinite(m.normalization.stddev[i]) ||
            m.normalization.stddev[i] <= 0)
            throw std::runtime_error("Invalid mean/std");
    if (json_object_has_member(root, "output")) {
        auto *out = required_object(root, "output");
        m.classes = integer(out, "num_classes", m.classes, 65536);
        m.top_k = integer(out, "top_k", 5, 100);
        m.keypoints = integer(out, "keypoints", 17, 64);
    }
    if (json_object_has_member(root, "labels")) {
        auto label_path =
            std::filesystem::path(path).parent_path() / required_string(root, "labels");
        std::ifstream f(label_path);
        if (!f)
            throw std::runtime_error("Cannot open labels: " + label_path.string());
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            m.labels.push_back(line);
        }
        if (m.labels.size() != m.classes)
            throw std::runtime_error("Label count differs from num_classes");
    } else
        for (unsigned i = 0; i < m.classes; i++)
            m.labels.push_back(std::to_string(i));
    return m;
}
struct ModelExecutor::Impl {
    int device{};
    unsigned batch{1};
    migraphx::program program;
    std::map<std::string, migraphx::shape> shapes;
    std::vector<DeviceTensor> inputs, outputs;
    std::map<std::string, std::shared_ptr<DeviceBuffer>> output_pool;
    std::mutex mutex;
};
ModelExecutor::ModelExecutor(const std::string &path, int device) {
    hip_check(hipSetDevice(device), "Model device");
    Parser manifest(path + ".manifest.json");
    auto *m = manifest.object();
    const auto schema = integer(m, "schema", 0, 2);
    if ((schema != 1 && schema != 2) || !json_object_has_member(m, "offload_copy") ||
        boolean(m, "offload_copy", true) || text(m, "gpu", "") != gpu_arch(device) ||
        text(m, "sha256", "") != sha256_file(path.c_str()))
        throw std::runtime_error("Incompatible model manifest: GPU, hash or device I/O");
    auto state = std::make_unique<Impl>();
    state->device = device;
    state->batch = integer(m, "batch", 1, 16);
    state->program = migraphx::load(path.c_str());
    auto shapes = state->program.get_parameter_shapes();
    for (const auto *name : shapes.names()) {
        state->shapes.emplace(name, shapes[name]);
        if (std::string(name).find("#output_") == std::string::npos)
            state->inputs.push_back(descriptor(name, shapes[name], device));
    }
    auto outputs = state->program.get_output_shapes();
    for (unsigned i = 0; i < outputs.size(); i++)
        state->outputs.push_back(descriptor("output" + std::to_string(i), outputs[i], device));
    if (state->inputs.empty() || state->outputs.empty())
        throw std::runtime_error("Model has no input/output");
    if (schema == 1 && !json_object_has_member(m, "batch")) {
        if (state->inputs[0].shape.empty() || state->inputs[0].shape[0] > 16)
            throw std::runtime_error("Cannot derive legacy model batch");
        state->batch = unsigned(state->inputs[0].shape[0]);
    }
    impl_ = std::move(state);
}
ModelExecutor::~ModelExecutor() {
    if (impl_) {
        (void)hipSetDevice(impl_->device);
        (void)hipDeviceSynchronize();
    }
}
std::vector<DeviceTensor> ModelExecutor::inputs() const { return impl_->inputs; }
std::vector<DeviceTensor> ModelExecutor::outputs() const { return impl_->outputs; }
unsigned ModelExecutor::batch() const { return impl_->batch; }
std::vector<DeviceTensor> ModelExecutor::infer(const std::map<std::string, DeviceTensor> &inputs) {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    hip_check(hipSetDevice(s.device), "Inference device");
    if (inputs.size() != s.inputs.size())
        throw std::runtime_error("Incorrect number of model inputs");
    migraphx::program_parameters params;
    for (const auto &expected : s.inputs) {
        auto it = inputs.find(expected.name);
        if (it == inputs.end() || it->second.type != expected.type ||
            it->second.shape != expected.shape || it->second.device != s.device ||
            !it->second.storage || it->second.storage->size() != s.shapes.at(expected.name).bytes())
            throw std::runtime_error("Model input mismatch: " + expected.name);
        device_pointer(it->second.storage->data(), s.device);
        params.add(expected.name.c_str(),
                   migraphx::argument(s.shapes.at(expected.name), it->second.storage->data()));
    }
    std::vector<std::shared_ptr<DeviceBuffer>> bindings;
    for (const auto &[name, shape] : s.shapes)
        if (name.find("#output_") != std::string::npos) {
            auto &memory = s.output_pool[name];
            if (!memory || memory.use_count() > 1)
                memory = std::make_shared<DeviceBuffer>(shape.bytes());
            params.add(name.c_str(), migraphx::argument(shape, memory->data()));
            bindings.push_back(memory);
        }
    auto outputs = s.program.eval(params);
    hip_check(hipDeviceSynchronize(), "Model completion");
    auto result = s.outputs;
    for (unsigned i = 0; i < outputs.size(); i++) {
        device_pointer(outputs[i].data(), s.device);
        for (auto &memory : bindings)
            if (memory->data() == outputs[i].data())
                result[i].storage = memory;
        if (!result[i].storage) {
            result[i].storage =
                std::make_shared<DeviceBuffer>(s.program.get_output_shapes()[i].bytes());
            hip_check(hipMemcpy(result[i].storage->data(), outputs[i].data(),
                                result[i].storage->size(), hipMemcpyDeviceToDevice),
                      "Owned model output");
        }
    }
    return result;
}
} // namespace amdva
