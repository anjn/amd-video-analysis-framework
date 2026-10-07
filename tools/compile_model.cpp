#include "amdva/model.hpp"
#include <fstream>
#include <glib.h>
#include <iostream>
#include <map>
#include <migraphx/migraphx.hpp>
#include <sstream>
int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: amdva-compile-model MODEL.onnx OUTPUT.mxr [--batch=N] "
                     "[--model-info=FILE] [--input-shape=NAME:D1,D2,...] [--precision=fp16|fp32]\n";
        return 1;
    }
    try {
        unsigned batch = 1;
        std::string info, precision = "fp16";
        std::map<std::string, std::vector<size_t>> input_shapes;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg.rfind("--batch=", 0) == 0) {
                size_t used = 0;
                auto value = arg.substr(8);
                batch = std::stoul(value, &used);
                if (used != value.size())
                    throw std::runtime_error("Invalid batch");
            } else if (arg.rfind("--model-info=", 0) == 0)
                info = arg.substr(13);
            else if (arg.rfind("--precision=", 0) == 0)
                precision = arg.substr(12);
            else if (arg.rfind("--input-shape=", 0) == 0) {
                auto value = arg.substr(14);
                auto colon = value.find(':');
                if (colon == std::string::npos || colon == 0)
                    throw std::runtime_error("Expected NAME:D1,D2,...");
                std::istringstream dims(value.substr(colon + 1));
                std::string dim;
                std::vector<size_t> shape;
                while (std::getline(dims, dim, ',')) {
                    size_t used = 0;
                    auto n = std::stoull(dim, &used);
                    if (used != dim.size() || !n || n > 1048576)
                        throw std::runtime_error("Invalid input dimension");
                    shape.push_back(n);
                }
                if (shape.empty() || shape.size() > 8)
                    throw std::runtime_error("Invalid input rank");
                input_shapes[value.substr(0, colon)] = shape;
            } else
                throw std::runtime_error("Unknown compiler option: " + arg);
        }
        if (batch != 1 && batch != 2 && batch != 4 && batch != 8 && batch != 16)
            throw std::runtime_error("Invalid batch size");
        if (precision != "fp16" && precision != "fp32")
            throw std::runtime_error("Expected fp16 or fp32 precision");
        if (!info.empty()) {
            auto model = amdva::ModelInfo::load(info);
            input_shapes[model.input_name] = {batch, 3, model.height, model.width};
        }
        const bool legacy = info.empty() && input_shapes.empty();
        if (legacy)
            input_shapes["images"] = {batch, 3, 640, 640};
        const auto arch = amdva::gpu_arch(0);
        migraphx::onnx_options options;
        options.set_default_dim_value(1);
        for (const auto &[name, shape] : input_shapes)
            options.set_input_parameter_shape(name.c_str(), shape);
        auto program = migraphx::parse_onnx(argv[1], options);
        auto shapes = program.get_parameter_shapes();
        for (const auto &[name, shape] : input_shapes)
            if (shapes[name.c_str()].lengths() != shape)
                throw std::runtime_error("ONNX input shape mismatch: " + name);
        auto outputs = program.get_output_shapes();
        if (legacy &&
            (outputs.size() != 1 || outputs[0].lengths() != std::vector<size_t>{batch, 84, 8400}))
            throw std::runtime_error("Legacy mode expects YOLOv8 output; provide --model-info or "
                                     "--input-shape for other models");
        if (precision == "fp16")
            migraphx::quantize_fp16(program);
        migraphx::compile_options compile;
        compile.set_offload_copy(false);
        program.compile(migraphx::target("gpu"), compile);
        migraphx::save(program, argv[2]);
        gchar *data{};
        gsize size{};
        if (!g_file_get_contents(argv[2], &data, &size, nullptr))
            throw std::runtime_error("Cannot hash compiled model");
        gchar *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                  reinterpret_cast<const guchar *>(data), size);
        g_free(data);
        std::ofstream manifest(std::string(argv[2]) + ".manifest.json");
        manifest << "{\"schema\":2,\"gpu\":\"" << arch
                 << "\",\"offload_copy\":false,\"precision\":\"" << precision
                 << "\",\"batch\":" << batch << ",\"sha256\":\"" << hash << "\"}\n";
        g_free(hash);
        if (!manifest)
            throw std::runtime_error("Cannot write model manifest");
        program.print();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Model preparation failed: " << e.what() << "\n";
        return 2;
    }
}
