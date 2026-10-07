#include "amdva/batching.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
using namespace amdva;
static void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
static DeviceTensor image(int cls = 0) {
    std::vector<float> values(3 * 8 * 8, 0);
    for (unsigned i = cls * 64; i < (cls + 1) * 64; i++)
        values[i] = 5;
    auto memory = std::make_shared<DeviceBuffer>(values.size() * sizeof(float));
    hip_check(hipMemcpy(memory->data(), values.data(), memory->size(), hipMemcpyHostToDevice),
              "Fixture upload");
    return {"images", TensorType::Float32, {1, 3, 8, 8}, memory, 0};
}
static std::vector<float> read(const DeviceTensor &tensor) {
    std::vector<float> result(tensor.elements());
    hip_check(hipMemcpy(result.data(), tensor.storage->data(), result.size() * sizeof(float),
                        hipMemcpyDeviceToHost),
              "Offline reference readback");
    return result;
}
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: model-executor-tests FIXTURE_DIRECTORY\n";
        return 1;
    }
    try {
        std::filesystem::path dir = argv[1];
        std::vector<DeviceTensor> retained;
        {
            ModelExecutor engine((dir / "multi.mxr").string());
            auto inputs = engine.inputs();
            require(inputs.size() == 2, "Named multi-input descriptors");
            auto img = image(1);
            DeviceTensor bias{"bias",
                              TensorType::Float32,
                              {1, 3},
                              std::make_shared<DeviceBuffer>(3 * sizeof(float)),
                              0};
            float values[] = {1, 2, 3};
            hip_check(
                hipMemcpy(bias.storage->data(), values, sizeof(values), hipMemcpyHostToDevice),
                "Bias upload");
            retained = engine.infer({{"images", img}, {"bias", bias}});
            require(retained.size() == 2, "Multiple output descriptors");
            auto before = read(retained[0]);
            img = image(2);
            auto second = engine.infer({{"images", img}, {"bias", bias}});
            require(read(retained[0]) == before, "Output was overwritten by the next inference");
            require(read(second[0]) != before, "Named input was ignored");
            bool rejected = false;
            try {
                engine.infer({{"images", img}});
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Missing named input accepted");
        }
        require(read(retained[0]) == std::vector<float>({1, 7, 3}),
                "Output ownership after model destruction");
        for (unsigned size : {1u, 2u, 4u, 8u, 16u}) {
            BatchOptions o;
            o.model = (dir / ("batch" + std::to_string(size) + ".mxr")).string();
            o.model_info = (dir / "model.json").string();
            o.size = size;
            o.instance_id = "test/" + std::to_string(size);
            auto batch = SharedBatchExecutor::acquire(o);
            require(batch == SharedBatchExecutor::acquire(o), "Executor is not shared");
            auto incompatible = o;
            ++incompatible.timeout_ms;
            bool rejected = false;
            try {
                SharedBatchExecutor::acquire(incompatible);
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Incompatible shared settings accepted");
            std::vector<InferenceRequest> requests;
            for (unsigned i = 0; i < size; i++)
                requests.push_back({image(i % 3), "camera/" + std::to_string(i), 1, i});
            auto results = batch->submit_many(std::move(requests));
            for (unsigned i = 0; i < size; i++) {
                auto r = results[i].get();
                require(!r.classifications.empty() && r.classifications[0].class_id == int(i % 3),
                        "Batch source isolation");
            }
            auto start = std::chrono::steady_clock::now();
            auto partial = batch->submit({image(2), "partial", 2, 0});
            require(partial.get().classifications[0].class_id == 2, "Partial batch result");
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(3),
                    "Partial batch did not time out");
            batch->cancel("partial", 2);
            rejected = false;
            try {
                batch->submit({image(), "partial", 2, 1});
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Retired generation accepted");
            require(batch->submit({image(1), "partial", 3, 2}).get().classifications[0].class_id ==
                        1,
                    "Replacement source generation rejected");
            batch->release_source("partial", 3);
        }
        {
            BatchOptions o;
            o.model = (dir / "batch8.mxr").string();
            o.model_info = (dir / "model.json").string();
            o.size = 8;
            o.timeout_ms = 500;
            o.queue_limit = 2;
            auto batch = SharedBatchExecutor::acquire(o);
            auto future = batch->submit({image(), "retired", 7, 0});
            batch->cancel("retired", 7);
            bool rejected = false;
            try {
                future.get();
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Queued cancellation was ignored");
            rejected = false;
            try {
                batch->submit_many(
                    {{image(), "a", 1, 0}, {image(), "b", 1, 0}, {image(), "c", 1, 0}});
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Batch queue limit was ignored");
        }
        std::cout << "Named GPU I/O, output ownership, batches 1/2/4/8/16, partial timeout, source "
                     "isolation, cancellation and queue limits passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
