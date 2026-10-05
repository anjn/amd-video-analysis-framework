#pragma once
#include "amdva/runtime.hpp"
#include <gst/gst.h>
#include <memory>
namespace amdva {
struct Tensor {
    int device{};
    unsigned width{640},height{640},original_width{},original_height{};
    unsigned scaled_width{},scaled_height{},pad_x{},pad_y{};
    std::shared_ptr<DeviceBuffer> buffer;
    hipEvent_t ready{};
    ~Tensor() { if(ready) { (void)hipSetDevice(device); (void)hipEventSynchronize(ready); (void)hipEventDestroy(ready); } }
};
struct TensorMeta { GstMeta meta; std::shared_ptr<Tensor>* tensor; };
GType tensor_meta_api();
const GstMetaInfo* tensor_meta_info();
TensorMeta* add_tensor_meta(GstBuffer* buffer, std::shared_ptr<Tensor> tensor);
TensorMeta* get_tensor_meta(GstBuffer* buffer);
} // namespace amdva
