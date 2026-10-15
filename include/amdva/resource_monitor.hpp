#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace amdva {
// Percentages are system-wide except VPE (observable DRM clients only).
// Memory is binary GiB, bandwidth decimal GB/s, power W. Missing means unknown.
struct ResourceSample {
    int64_t monotonic_ns{}, wall_ms{};
    std::optional<double> cpu_pct, gpu_pct, dram_read_gbs, dram_write_gbs;
    std::optional<double> host_used_gib, host_total_gib, vram_reserved_gib, vram_gib;
    std::optional<double> gtt_gib, gtt_total_gib, socket_w, vcn_pct, vpe_pct;
};
struct ResourceMonitorConfig {
    std::string render_node{"/dev/dri/renderD128"};
    std::string proc_root{"/proc"}, sys_root{"/sys"};
};
// Single caller; does not spawn threads or change system configuration.
class ResourceMonitor {
  public:
    explicit ResourceMonitor(ResourceMonitorConfig = {});
    ~ResourceMonitor();
    ResourceMonitor(const ResourceMonitor &) = delete;
    ResourceMonitor &operator=(const ResourceMonitor &) = delete;
    ResourceSample sample();
    const std::string &cpu_name() const;
    const std::string &gpu_name() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Decode the validated v3.0 ABI. Other versions/truncated blobs remain absent.
void decode_gpu_metrics(const std::vector<uint8_t> &, ResourceSample &);
} // namespace amdva
