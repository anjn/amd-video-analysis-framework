#include "amdva/resource_monitor.hpp"
#include <algorithm>
#include <amdgpu.h>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>
namespace amdva {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
std::string read(const fs::path &p) {
    try {
        std::ifstream f(p, std::ios::binary);
        if (!f)
            return {};
        auto value = std::string(std::istreambuf_iterator<char>(f), {});
        return f.bad() ? std::string{} : value;
    } catch (const std::ios_base::failure &) {
        // procfs entries may disappear between open and read as clients exit.
        return {};
    }
}

std::optional<double> number(const fs::path &p, double scale = 1) {
    std::istringstream f(read(p));
    double value{};
    if (!(f >> value) || !std::isfinite(value) || value < 0)
        return {};
    return value / scale;
}
std::optional<double> percent(const fs::path &p) {
    auto n = number(p);
    return n && *n <= 100 ? n : std::nullopt;
}
std::map<std::string, std::string> fields(const std::string &text) {
    std::map<std::string, std::string> out;
    std::istringstream f(text);
    std::string s;
    while (std::getline(f, s)) {
        auto pos = s.find(':');
        if (pos == std::string::npos)
            continue;
        auto value = s.substr(pos + 1);
        auto start = value.find_first_not_of(" \t");
        auto key = s.substr(0, pos);
        auto end = key.find_last_not_of(" \t");
        if (end != std::string::npos)
            key.resize(end + 1);
        out[key] = start == std::string::npos ? "" : value.substr(start);
    }
    return out;
}
uint64_t count(const std::string &s) {
    std::istringstream f(s);
    uint64_t v{};
    f >> v;
    return v;
}
bool digits(const std::string &s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}
uint64_t little(const std::vector<uint8_t> &b, unsigned offset, unsigned bytes) {
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++)
        v |= uint64_t(b[offset + i]) << (8 * i);
    return v;
}
} // namespace
void decode_gpu_metrics(const std::vector<uint8_t> &b, ResourceSample &s) {
    s.dram_read_gbs.reset();
    s.dram_write_gbs.reset();
    s.socket_w.reset();
    // gpu_metrics_v3_0, kgd_pp_interface.h: header size includes tail padding.
    if (b.size() < 264 || little(b, 0, 2) != 264 || b[2] != 3 || b[3] != 0)
        return;
    const auto reads = little(b, 94, 2), writes = little(b, 96, 2), socket = little(b, 112, 4);
    if (reads != 0xffff)
        s.dram_read_gbs = double(reads) / 1000;
    if (writes != 0xffff)
        s.dram_write_gbs = double(writes) / 1000;
    if (socket != 0xffffffff)
        s.socket_w = double(socket) / 1000;
}
struct ResourceMonitor::Impl {
    ResourceMonitorConfig config;
    fs::path device;
    std::string pci, cpu{"CPU"}, gpu{"AMD GPU"};
    uint64_t total{}, idle{};
    bool have_cpu{};
    int64_t last_ns{};
    std::map<std::string, uint64_t> previous;
    explicit Impl(ResourceMonitorConfig c) : config(std::move(c)) {
        std::error_code error;
        device = fs::canonical(fs::path(config.sys_root) / "class/drm" /
                                   fs::path(config.render_node).filename() / "device",
                               error);
        if (error)
            throw std::runtime_error("Cannot identify DRM device for " + config.render_node);
        pci = device.filename().string();
        auto info = fields(read(fs::path(config.proc_root) / "cpuinfo"));
        if (info.count("model name"))
            cpu = info["model name"];
        const int fd = open(config.render_node.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            amdgpu_device_handle handle{};
            uint32_t major{}, minor{};
            if (amdgpu_device_initialize(fd, &major, &minor, &handle) == 0) {
                if (auto *name = amdgpu_get_marketing_name(handle))
                    gpu = name;
                amdgpu_device_deinitialize(handle);
            }
            close(fd);
        }
        if (gpu == "AMD GPU") {
            auto name = read(device / "product_name");
            if (!name.empty()) {
                while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
                    name.pop_back();
                gpu = name;
            }
        }
    }
    std::map<std::string, double> engines(int64_t now) {
        // Scan once for both video and VPE. Each engine has its own capacity;
        // distinct video engine classes may execute concurrently.
        std::map<std::string, uint64_t> current;
        std::map<std::string, uint64_t> capacities;
        std::error_code error;
        for (fs::directory_iterator
                 it(config.proc_root, fs::directory_options::skip_permission_denied, error),
             end;
             it != end; it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (!digits(it->path().filename().string()))
                continue;
            std::error_code fd_error;
            for (fs::directory_iterator f(it->path() / "fdinfo",
                                          fs::directory_options::skip_permission_denied, fd_error),
                 last;
                 f != last; f.increment(fd_error)) {
                if (fd_error) {
                    fd_error.clear();
                    continue;
                }
                auto data = fields(read(f->path()));
                if (data["drm-driver"] != "amdgpu" || data["drm-pdev"] != pci ||
                    !digits(data["drm-client-id"]))
                    continue;
                for (const std::string engine : {"enc", "dec", "enc_1", "vpe"}) {
                    const auto counter = data.find("drm-engine-" + engine);
                    if (counter == data.end())
                        continue;
                    uint64_t ns{};
                    std::string unit;
                    std::istringstream value(counter->second);
                    if (counter->second.empty() || counter->second.front() < '0' ||
                        counter->second.front() > '9' || !(value >> ns >> unit) || unit != "ns")
                        continue;
                    const auto key = engine + ":" + pci + ":" + data["drm-client-id"];
                    current[key] = std::max(current[key], ns);
                    capacities[engine] = std::max<uint64_t>(
                        {capacities[engine], 1, count(data["drm-engine-capacity-" + engine])});
                }
            }
        }
        std::map<std::string, double> deltas;
        for (auto &[id, value] : current) {
            auto found = previous.find(id);
            if (found == previous.end())
                continue;
            // Hold the high watermark for temporarily non-monotonic counters.
            if (value < found->second) {
                value = found->second;
                continue;
            }
            deltas[id.substr(0, id.find(':'))] += double(value - found->second);
        }
        std::map<std::string, double> result;
        if (last_ns && now > last_ns)
            for (auto [engine, delta] : deltas)
                result[engine] = std::clamp(
                    100.0 * delta / double(now - last_ns) / capacities.at(engine), 0., 100.);
        previous = std::move(current);
        last_ns = now;
        return result;
    }
    ResourceSample sample() {
        ResourceSample s;
        s.monotonic_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
                .count();
        s.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
        std::istringstream f(read(fs::path(config.proc_root) / "stat"));
        std::string label;
        f >> label;
        uint64_t values[8]{};
        bool complete = label == "cpu";
        for (auto &v : values)
            if (!(f >> v))
                complete = false;
        uint64_t sum = 0;
        for (auto v : values)
            sum += v;
        const auto inactive = values[3] + values[4];
        if (complete) {
            if (have_cpu && sum > total && inactive >= idle && inactive - idle <= sum - total)
                s.cpu_pct = 100.0 * (1.0 - double(inactive - idle) / double(sum - total));
            total = sum;
            idle = inactive;
            have_cpu = true;
        } else
            have_cpu = false;
        constexpr double gib = 1073741824.;
        const auto mem = fields(read(fs::path(config.proc_root) / "meminfo"));
        if (mem.count("MemTotal") && mem.count("MemAvailable")) {
            const auto t = count(mem.at("MemTotal")), a = count(mem.at("MemAvailable"));
            if (t && a <= t) {
                s.host_total_gib = double(t) * 1024 / gib;
                s.host_used_gib = double(t - a) * 1024 / gib;
            }
        }
        s.gpu_pct = percent(device / "gpu_busy_percent");
        s.vcn_pct = percent(device / "vcn_busy_percent");
        s.vram_reserved_gib = number(device / "mem_info_vram_total", gib);
        s.vram_gib = number(device / "mem_info_vram_used", gib);
        s.gtt_total_gib = number(device / "mem_info_gtt_total", gib);
        s.gtt_gib = number(device / "mem_info_gtt_used", gib);
        auto raw = read(device / "gpu_metrics");
        decode_gpu_metrics(std::vector<uint8_t>(raw.begin(), raw.end()), s);
        const auto busy = engines(s.monotonic_ns);
        if (auto it = busy.find("vpe"); it != busy.end())
            s.vpe_pct = it->second;
        std::optional<double> video;
        // VCN4 exposes both encode and decode on the unified "enc" engine.
        // On older devices show the busiest video class, never their sum:
        // independent encode/decode engines can overlap. Prefer DRM even when
        // its measured usage is zero; sysfs VCN is stuck at zero on some APUs.
        for (const auto *engine : {"enc", "dec", "enc_1"})
            if (auto it = busy.find(engine); it != busy.end())
                video = std::max(video.value_or(0.), it->second);
        if (video)
            s.vcn_pct = video;
        return s;
    }
};
ResourceMonitor::ResourceMonitor(ResourceMonitorConfig c)
    : impl_(std::make_unique<Impl>(std::move(c))) {}
ResourceMonitor::~ResourceMonitor() = default;
ResourceSample ResourceMonitor::sample() { return impl_->sample(); }
const std::string &ResourceMonitor::cpu_name() const { return impl_->cpu; }
const std::string &ResourceMonitor::gpu_name() const { return impl_->gpu; }
} // namespace amdva
