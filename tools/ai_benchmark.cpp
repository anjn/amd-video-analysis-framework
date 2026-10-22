#include "amdva/model.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sys/resource.h>
using namespace amdva;
int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: amdva-ai-benchmark MODEL.mxr [SECONDS=30] [REPEATS=3] [INPUT.f32]\n";
        return 2;
    }
    try {
        using Clock = std::chrono::steady_clock;
        auto start = Clock::now();
        ModelExecutor model(argv[1]);
        auto inputs = model.inputs();
        if (inputs.size() != 1 || inputs[0].type != TensorType::Float32)
            throw std::runtime_error("Expected one FP32 input");
        auto input = inputs[0];
        std::vector<float> host(input.elements(), .5f);
        if (argc > 4) {
            std::ifstream f(argv[4], std::ios::binary);
            if (!f.read(reinterpret_cast<char *>(host.data()), host.size() * sizeof(float)))
                throw std::runtime_error("Invalid input fixture");
        }
        input.storage = std::make_shared<DeviceBuffer>(host.size() * sizeof(float));
        hip_check(hipMemcpy(input.storage->data(), host.data(), host.size() * sizeof(float),
                            hipMemcpyHostToDevice),
                  "Benchmark input");
        const double load = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        for (int n = 0; n < 20; ++n)
            model.infer({{input.name, input}});
        const double seconds = argc > 2 ? std::stod(argv[2]) : 30;
        const int repeats = argc > 3 ? std::stoi(argv[3]) : 3;
        std::cout << "{\"model\":\"" << argv[1] << "\",\"load_ms\":" << load << ",\"shape\":[";
        for (size_t i = 0; i < input.shape.size(); ++i)
            std::cout << (i ? "," : "") << input.shape[i];
        std::cout << "],\"timing\":\"completed wall time, inference only\",\"runs\":[";
        for (int r = 0; r < repeats; ++r) {
            std::vector<double> values;
            auto until = Clock::now() + std::chrono::duration<double>(seconds);
            do {
                auto t = Clock::now();
                model.infer({{input.name, input}});
                values.push_back(
                    std::chrono::duration<double, std::milli>(Clock::now() - t).count());
            } while (values.size() < 200 || Clock::now() < until);
            std::sort(values.begin(), values.end());
            double mean = std::accumulate(values.begin(), values.end(), 0.) / values.size();
            std::cout << (r ? "," : "") << "{\"samples\":" << values.size()
                      << ",\"mean_ms\":" << mean << ",\"p50_ms\":" << values[values.size() / 2]
                      << ",\"p95_ms\":" << values[size_t(values.size() * .95)]
                      << ",\"max_ms\":" << values.back()
                      << ",\"per_image_ms\":" << mean / input.shape[0]
                      << ",\"images_per_second\":" << 1000 * input.shape[0] / mean << "}";
        }
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        std::cout << "],\"max_rss_mib\":" << usage.ru_maxrss / 1024.0 << "}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
