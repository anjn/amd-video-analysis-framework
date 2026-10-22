#include "amdva/model.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: amdva-ai-infer MODEL.mxr INPUT.f32 OUTPUT.f32\n";
        return 2;
    }
    try {
        amdva::ModelExecutor model(argv[1]);
        auto inputs = model.inputs(), outputs = model.outputs();
        if (inputs.size() != 1 || outputs.size() != 1 ||
            inputs[0].type != amdva::TensorType::Float32 ||
            outputs[0].type != amdva::TensorType::Float32)
            throw std::runtime_error("Expected one FP32 input and output");
        auto input = inputs.front();
        std::vector<float> host(input.elements()), out(outputs.front().elements());
        input.storage = std::make_shared<amdva::DeviceBuffer>(host.size() * sizeof(float));
        std::ifstream f(argv[2], std::ios::binary);
        std::ofstream dest(argv[3], std::ios::binary);
        if (!f || !dest)
            throw std::runtime_error("Cannot open fixture");
        while (f.peek() != EOF) {
            std::fill(host.begin(), host.end(), 0);
            f.read(reinterpret_cast<char *>(host.data()), host.size() * sizeof(float));
            auto count = f.gcount();
            if (count % sizeof(float) || count <= 0)
                throw std::runtime_error("Invalid FP32 fixture");
            amdva::hip_check(hipMemcpy(input.storage->data(), host.data(),
                                       host.size() * sizeof(float), hipMemcpyHostToDevice),
                             "Fixture input");
            auto result = model.infer({{input.name, input}});
            amdva::hip_check(hipMemcpy(out.data(), result[0].storage->data(),
                                       out.size() * sizeof(float), hipMemcpyDeviceToHost),
                             "Fixture output");
            dest.write(reinterpret_cast<char *>(out.data()), out.size() * sizeof(float));
            if (!dest)
                throw std::runtime_error("Cannot write output");
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
