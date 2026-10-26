#pragma once
#include <algorithm>
#include <cstddef>
#include <stdexcept>
namespace amdva::nvr {
// GPU handles a prefix and NPU the remaining (at most one) image. A lone
// image alternates devices across collection calls; queries use both encoders.
inline size_t mobileclip_gpu_count(size_t count, unsigned gpu_capacity, bool single_on_npu) {
    if ((gpu_capacity != 1 && gpu_capacity != 4) || !count || count > gpu_capacity + 1)
        throw std::invalid_argument("Invalid combined MobileCLIP batch");
    return count == 1 ? (single_on_npu ? 0 : 1) : std::min<size_t>(gpu_capacity, count - 1);
}
} // namespace amdva::nvr
