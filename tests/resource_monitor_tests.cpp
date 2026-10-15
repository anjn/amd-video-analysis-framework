#include "amdva/resource_monitor.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace amdva;
namespace fs = std::filesystem;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void write(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}
int main() {
    const auto root =
        fs::temp_directory_path() / ("amdva-monitor-test-" + std::to_string(getpid()));
    try {
        std::vector<uint8_t> bytes(264);
        bytes[0] = 8;
        bytes[1] = 1;
        bytes[2] = 3;
        auto set = [&](unsigned pos, uint64_t value, unsigned n) {
            for (unsigned i = 0; i < n; i++)
                bytes[pos + i] = (value >> (8 * i)) & 255;
        };
        set(94, 12345, 2);
        set(96, 6789, 2);
        set(112, 48000, 4);
        ResourceSample sample;
        decode_gpu_metrics(bytes, sample);
        require(sample.dram_read_gbs && std::abs(*sample.dram_read_gbs - 12.345) < 1e-8 &&
                    sample.socket_w == 48,
                "v3 metric units/offsets");
        set(94, 65535, 2);
        set(112, 0xffffffff, 4);
        decode_gpu_metrics(bytes, sample);
        require(!sample.dram_read_gbs && !sample.socket_w && sample.dram_write_gbs,
                "Unavailable metric sentinel");
        bytes[2] = 2;
        decode_gpu_metrics(bytes, sample);
        require(!sample.dram_write_gbs, "Unknown metric version");
        bytes[2] = 3;
        bytes.resize(116);
        decode_gpu_metrics(bytes, sample);
        require(!sample.socket_w, "Truncated metrics");
        auto proc = root / "proc", sys = root / "sys", device = root / "0000:65:00.0";
        fs::create_directories(sys / "class/drm/renderD128");
        fs::create_directories(device);
        fs::create_directory_symlink(device, sys / "class/drm/renderD128/device");
        write(proc / "cpuinfo", "model name\t: Test CPU\n");
        write(proc / "stat", "cpu 100 0 100 800 0 0 0 0\n");
        write(proc / "meminfo", "MemTotal: 31457280 kB\nMemAvailable: 20971520 kB\n");
        write(device / "gpu_busy_percent", "40\n");
        write(device / "vcn_busy_percent", "12\n");
        write(device / "mem_info_vram_total", "2147483648");
        write(device / "mem_info_vram_used", "536870912");
        write(device / "mem_info_gtt_total", "25769803776");
        write(device / "mem_info_gtt_used", "3221225472");
        auto client = [](uint64_t ns) {
            return "drm-driver: amdgpu\ndrm-pdev: 0000:65:00.0\ndrm-client-id: "
                   "1\ndrm-engine-vpe: " +
                   std::to_string(ns) + " ns\ndrm-engine-capacity-vpe: 2\n";
        };
        write(proc / "10/fdinfo/1", client(1000000));
        write(proc / "11/fdinfo/2", client(1000000));
        // Opening a directory as a file causes a read-time EISDIR, like a
        // procfs entry that opens successfully but disappears during reading.
        fs::create_directories(proc / "12/fdinfo/1");
        ResourceMonitor monitor({"/no/renderD128", proc.string(), sys.string()});
        auto first = monitor.sample();
        require(!first.cpu_pct && !first.vpe_pct, "First delta must be unavailable");
        require(first.host_total_gib == 30 && first.host_used_gib == 10 &&
                    first.gtt_total_gib == 24 && first.vram_reserved_gib == 2,
                "Memory units and separate capacities");
        require(monitor.cpu_name() == "Test CPU" && first.gpu_pct == 40 && first.vcn_pct == 12,
                "Names and independent metrics");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        write(proc / "stat", "cpu 150 0 100 850 0 0 0 0\n");
        write(proc / "10/fdinfo/1", client(2000000));
        write(proc / "11/fdinfo/2", client(2000000));
        auto second = monitor.sample();
        const auto expected = 100. * 1000000 / double(second.monotonic_ns - first.monotonic_ns) / 2;
        require(second.cpu_pct == 50 && second.vpe_pct &&
                    std::abs(*second.vpe_pct - expected) < .001,
                "CPU delta / VPE aliases and capacity");
        write(proc / "10/fdinfo/1", client(500000));
        write(proc / "11/fdinfo/2", client(500000));
        auto reset = monitor.sample();
        require(!reset.vpe_pct, "Counter regression must not become negative usage");
        write(proc / "10/fdinfo/1", client(2000000));
        write(proc / "11/fdinfo/2", client(2000000));
        require(monitor.sample().vpe_pct == 0, "Counter high watermark recovery");
        fs::remove_all(proc / "10");
        fs::remove_all(proc / "11");
        require(!monitor.sample().vpe_pct, "Exited clients");
        write(device / "gpu_busy_percent", "200");
        write(proc / "meminfo", "MemTotal: 20 kB\nMemAvailable: 30 kB\n");
        auto invalid = monitor.sample();
        require(!invalid.gpu_pct && !invalid.host_used_gib && invalid.vcn_pct == 12,
                "Invalid/partial metrics");
        write(proc / "stat", "bad");
        require(!monitor.sample().cpu_pct, "Broken proc stat");
        fs::remove_all(root);
        std::cout << "Metrics ABI, units, memory accounting, CPU deltas, DRM "
                     "deduplication/lifecycle passed\n";
        return 0;
    } catch (const std::exception &e) {
        fs::remove_all(root);
        std::cerr << e.what() << "\n";
        return 1;
    }
}
