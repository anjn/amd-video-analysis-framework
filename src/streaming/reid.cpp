#include "reid.hpp"
#include <algorithm>
#include <cmath>
extern "C" void amdva_audit_embedding_scope(bool) __attribute__((weak));
namespace amdva::streaming {
ReIdModel::ReIdModel(const std::string &path, unsigned device) : model_(path, device) {
    auto in = model_.inputs(), out = model_.outputs();
    if (in.size() != 1 || in[0].shape != std::vector<size_t>{1, 3, 256, 128} ||
        in[0].type != TensorType::Float32 || out.size() != 1 ||
        out[0].shape != std::vector<size_t>{1, 512} || out[0].type != TensorType::Float32)
        throw std::runtime_error("OSNet requires float32 [1,3,256,128] -> [1,512]");
    input_ = in[0];
    input_.storage = std::make_shared<DeviceBuffer>(3 * 256 * 128 * sizeof(float));
}
ReIdFeature ReIdModel::encode(const SearchImage &im) {
    if (!im.width || !im.height || im.rgb.size() != size_t(im.width) * im.height * 3)
        throw std::invalid_argument("Invalid ReID image");
    std::vector<float> pixels(3 * 256 * 128);
    const float mean[] = {.485f, .456f, .406f}, sd[] = {.229f, .224f, .225f};
    for (unsigned y = 0; y < 256; ++y)
        for (unsigned x = 0; x < 128; ++x) {
            float fx = std::clamp((x + .5f) * im.width / 128 - .5f, 0.f, float(im.width - 1));
            float fy = std::clamp((y + .5f) * im.height / 256 - .5f, 0.f, float(im.height - 1));
            unsigned x0 = fx, y0 = fy, x1 = std::min(x0 + 1, im.width - 1),
                     y1 = std::min(y0 + 1, im.height - 1);
            for (unsigned c = 0; c < 3; ++c) {
                auto at = [&](unsigned xx, unsigned yy) {
                    return im.rgb[(size_t(yy) * im.width + xx) * 3 + c];
                };
                float a = at(x0, y0) * (1 - (fx - x0)) + at(x1, y0) * (fx - x0),
                      b = at(x0, y1) * (1 - (fx - x0)) + at(x1, y1) * (fx - x0);
                pixels[c * 256 * 128 + y * 128 + x] =
                    ((a * (1 - (fy - y0)) + b * (fy - y0)) / 255 - mean[c]) / sd[c];
            }
        }
    struct Scope {
        Scope() {
            if (amdva_audit_embedding_scope)
                amdva_audit_embedding_scope(true);
        }
        ~Scope() {
            if (amdva_audit_embedding_scope)
                amdva_audit_embedding_scope(false);
        }
    } scope;
    hip_check(hipMemcpy(input_.storage->data(), pixels.data(), pixels.size() * sizeof(float),
                        hipMemcpyHostToDevice),
              "ReID input");
    auto result = model_.infer({{input_.name, input_}});
    ReIdFeature f;
    hip_check(hipMemcpy(f.data(), result[0].storage->data(), sizeof(f), hipMemcpyDeviceToHost),
              "ReID feature");
    float norm = 0;
    for (float x : f) {
        if (!std::isfinite(x))
            throw std::runtime_error("Non-finite ReID feature");
        norm += x * x;
    }
    if (norm < 1e-12f)
        throw std::runtime_error("Empty ReID feature");
    for (auto &x : f)
        x /= std::sqrt(norm);
    return f;
}
} // namespace amdva::streaming
