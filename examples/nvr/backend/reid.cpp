#include "backend/reid.hpp"
#include <algorithm>
#include <cmath>
extern "C" void amdva_audit_embedding_scope(bool) __attribute__((weak));
namespace amdva::nvr {
ReIdModel::ReIdModel(const std::string &path, unsigned device) : model_(path, device) {
    auto in = model_.inputs(), out = model_.outputs();
    if (in.size() != 1 || in[0].shape.size() != 4 || in[0].shape[1] != 3 || in[0].shape[0] < 1 ||
        in[0].shape[0] > 16 || in[0].shape[2] < 64 || in[0].shape[2] > 256 || in[0].shape[3] < 32 ||
        in[0].shape[3] > 128 || in[0].type != TensorType::Float32 || out.size() != 1 ||
        out[0].shape != std::vector<size_t>{in[0].shape[0], 512} ||
        out[0].type != TensorType::Float32)
        throw std::runtime_error("Invalid OSNet input/output contract");
    input_ = in[0];
    input_.storage = std::make_shared<DeviceBuffer>(input_.elements() * sizeof(float));
}
ReIdFeature ReIdModel::encode(const SearchImage &im) { return encode_many({im}).front(); }
std::vector<ReIdFeature> ReIdModel::encode_many(const std::vector<SearchImage> &images) {
    using Clock = std::chrono::steady_clock;
    prepare_ms = inference_ms = 0;
    std::vector<ReIdFeature> result;
    const unsigned batch = input_.shape[0], height = input_.shape[2], width = input_.shape[3];
    for (size_t offset = 0; offset < images.size(); offset += batch) {
        auto start = Clock::now();
        std::vector<float> pixels(input_.elements());
        unsigned count = std::min<size_t>(batch, images.size() - offset);
        const float mean[] = {.485f, .456f, .406f}, sd[] = {.229f, .224f, .225f};
        for (unsigned n = 0; n < batch; ++n) {
            const auto &im = images[offset + std::min(n, count - 1)];
            if (!im.width || !im.height || im.rgb.size() != size_t(im.width) * im.height * 3)
                throw std::invalid_argument("Invalid ReID image");
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x) {
                    float fx = std::clamp((x + .5f) * im.width / width - .5f, 0.f,
                                          float(im.width - 1)),
                          fy = std::clamp((y + .5f) * im.height / height - .5f, 0.f,
                                          float(im.height - 1));
                    unsigned x0 = fx, y0 = fy, x1 = std::min(x0 + 1, im.width - 1),
                             y1 = std::min(y0 + 1, im.height - 1);
                    for (unsigned c = 0; c < 3; ++c) {
                        auto at = [&](unsigned xx, unsigned yy) {
                            return im.rgb[(size_t(yy) * im.width + xx) * 3 + c];
                        };
                        float a = at(x0, y0) * (1 - (fx - x0)) + at(x1, y0) * (fx - x0),
                              b = at(x0, y1) * (1 - (fx - x0)) + at(x1, y1) * (fx - x0);
                        pixels[((n * 3 + c) * height + y) * width + x] =
                            ((a * (1 - (fy - y0)) + b * (fy - y0)) / 255 - mean[c]) / sd[c];
                    }
                }
        }
        prepare_ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
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
        start = Clock::now();
        hip_check(hipMemcpy(input_.storage->data(), pixels.data(), pixels.size() * sizeof(float),
                            hipMemcpyHostToDevice),
                  "ReID input");
        auto output = model_.infer({{input_.name, input_}});
        std::vector<ReIdFeature> features(batch);
        hip_check(hipMemcpy(features.data(), output[0].storage->data(),
                            features.size() * sizeof(ReIdFeature), hipMemcpyDeviceToHost),
                  "ReID features");
        inference_ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        for (unsigned n = 0; n < count; ++n) {
            auto &f = features[n];
            float norm = 0;
            for (float v : f) {
                if (!std::isfinite(v))
                    throw std::runtime_error("Non-finite ReID feature");
                norm += v * v;
            }
            if (norm < 1e-12f)
                throw std::runtime_error("Empty ReID feature");
            for (auto &v : f)
                v /= std::sqrt(norm);
            result.push_back(f);
        }
    }
    return result;
}
} // namespace amdva::nvr
