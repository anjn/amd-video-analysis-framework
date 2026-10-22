#include "../src/streaming/reid.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
using namespace amdva::streaming;
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: amdva-reid-validate MODEL.mxr REFERENCE.rgb REFERENCE.f32\n";
        return 2;
    }
    try {
        SearchImage image{128, 256, std::vector<uint8_t>(128 * 256 * 3)};
        ReIdFeature expected{};
        auto read = [](const char *p, void *data, size_t bytes) {
            std::ifstream f(p, std::ios::binary);
            if (!f.read(static_cast<char *>(data), bytes) || f.peek() != EOF)
                throw std::runtime_error("Invalid reference file");
        };
        read(argv[2], image.rgb.data(), image.rgb.size());
        read(argv[3], expected.data(), sizeof(expected));
        ReIdModel model(argv[1], 0);
        auto actual = model.encode(image);
        double cosine = reid_similarity(actual, expected);
        if (cosine < .999)
            throw std::runtime_error("OSNet CPU/GPU cosine mismatch");
        std::vector<double> elapsed;
        for (unsigned i = 0; i < 30; ++i) {
            auto start = std::chrono::steady_clock::now();
            model.encode(image);
            elapsed.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        }
        std::sort(elapsed.begin(), elapsed.end());
        std::cout << "PASS OSNet CPU/GPU cosine=" << cosine << " median_ms=" << elapsed[15]
                  << " p95_ms=" << elapsed[28] << "\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
