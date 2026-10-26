#include "amdva/inference.hpp"
#include <fstream>
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 5) {
        std::cerr << "Usage: amdva-batch-reference B4.mxr B1.mxr INFO.json INPUT.f32\n";
        return 1;
    }
    try {
        constexpr size_t image = 3 * 640 * 640;
        std::vector<float> host(4 * image);
        std::ifstream f(argv[4], std::ios::binary);
        f.read(reinterpret_cast<char *>(host.data()), host.size() * sizeof(float));
        if (size_t(f.gcount()) != host.size() * sizeof(float))
            throw std::runtime_error("Expected four input tensors");
        amdva::DeviceBuffer input(host.size() * sizeof(float));
        amdva::hip_check(hipMemcpy(input.data(), host.data(), input.size(), hipMemcpyHostToDevice),
                         "Offline fixture upload");
        amdva::BatchEngine batch(argv[1], argv[3], 4), single(argv[2], argv[3], 1);
        std::cout << "[";
        bool first_case = true;
        for (unsigned valid = 1; valid <= 4; ++valid) {
            auto detections = (valid == 1 ? single : batch).infer(input.data(), valid);
            if (!first_case)
                std::cout << ",";
            first_case = false;
            std::cout << "{\"valid\":" << valid << ",\"images\":[";
            for (unsigned i = 0; i < valid; ++i) {
                if (i)
                    std::cout << ",";
                std::cout << "[";
                bool first = true;
                for (const auto &b : detections[i]) {
                    if (!first)
                        std::cout << ",";
                    first = false;
                    std::cout << "[" << b.x << "," << b.y << "," << b.w << "," << b.h << ","
                              << b.score << "," << b.class_id << "]";
                }
                std::cout << "]";
            }
            std::cout << "]}";
        }
        std::cout << "]\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
