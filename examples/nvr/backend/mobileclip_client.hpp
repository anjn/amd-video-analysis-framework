#pragma once
#include "backend/search.hpp"
#include "backend/mobileclip_ipc.hpp"
#include <algorithm>
#include <csignal>
#include <filesystem>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
namespace amdva::nvr {
class MobileClipClient {
    int socket_{-1};
    pid_t pid_{-1};
    std::vector<float> response(size_t expected) {
        const auto status = mc2ipc::read(socket_), bytes = mc2ipc::read(socket_);
        if (status) {
            if (bytes > 65536)
                throw std::runtime_error("Invalid worker error");
            std::string error(bytes, '\0');
            mc2ipc::transfer(socket_, error.data(), bytes, false);
            throw std::runtime_error(error);
        }
        if (bytes != expected * sizeof(float))
            throw std::runtime_error("Invalid MobileCLIP output size");
        std::vector<float> values(expected);
        if (bytes)
            mc2ipc::transfer(socket_, values.data(), bytes, false);
        return values;
    }
    void stop() {
        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
            pid_ = -1;
        }
    }

  public:
    explicit MobileClipClient(const SearchOptions &o) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair))
            throw std::runtime_error("Cannot create MobileCLIP socket");
        socket_ = pair[0];
        std::string worker = o.mobileclip_worker;
        if (worker.empty()) {
            auto installed = std::filesystem::read_symlink("/proc/self/exe").parent_path() /
                             "amdva-mobileclip-worker";
            worker =
                std::filesystem::exists(installed) ? installed.string() : AMDVA_MOBILECLIP_WORKER;
        }
        // Positional arguments avoid shell interpolation of user-configured paths.
        std::vector<std::string> args;
        if (o.mobileclip_backend == "npu") {
            args = {
                "bash",
                "--noprofile",
                "--norc",
                "-c",
                R"(export LD_LIBRARY_PATH="${ROCM_PATH:+$ROCM_PATH/lib:$ROCM_PATH/lib/migraphx/lib}"; source /opt/xilinx/xrt/setup.sh >/dev/null && source "$1/bin/activate" >/dev/null && exec "$2" "$3" "$4" npu)",
                "mobileclip-worker",
                o.ryzen_ai_venv,
                worker,
                o.mobileclip_library,
                o.mobileclip_bundle};
        } else
            args = {worker, o.mobileclip_library, o.mobileclip_bundle, "gpu"};
        std::vector<char *> argv;
        for (auto &arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        // Close the parent's end before assigning fd 3 (socketpair commonly returns 3,4).
        posix_spawn_file_actions_addclose(&actions, pair[0]);
        posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
        if (pair[1] != 3)
            posix_spawn_file_actions_addclose(&actions, pair[1]);
        posix_spawn_file_actions_addclosefrom_np(&actions, 4);
        const int rc = posix_spawnp(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(pair[1]);
        if (rc) {
            stop();
            throw std::runtime_error("Cannot start MobileCLIP worker");
        }
        try {
            response(0);
        } catch (...) {
            stop();
            throw;
        }
    }
    ~MobileClipClient() { stop(); }
    std::vector<std::array<float, 768>> images(const std::vector<SearchImage> &images) {
        if (images.empty() || images.size() > 4)
            throw std::invalid_argument("MobileCLIP batch must contain 1 to 4 images");
        for (const auto &im : images)
            if (!im.width || !im.height || im.width > 1024 || im.height > 1024 ||
                im.rgb.size() != size_t(im.width) * im.height * 3)
                throw std::invalid_argument("Invalid MobileCLIP crop");
        mc2ipc::write(socket_, 2);
        mc2ipc::write(socket_, images.size());
        for (const auto &im : images) {
            mc2ipc::write(socket_, im.width);
            mc2ipc::write(socket_, im.height);
            mc2ipc::transfer(socket_, const_cast<uint8_t *>(im.rgb.data()), im.rgb.size(), true);
        }
        auto values = response(images.size() * 512);
        std::vector<std::array<float, 768>> out(images.size());
        for (size_t i = 0; i < images.size(); ++i)
            std::copy_n(values.data() + i * 512, 512, out[i].data());
        return out;
    }
    std::array<float, 768> text(const std::string &text) {
        if (text.empty() || text.size() > 65536 || text.find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid MobileCLIP text");
        mc2ipc::write(socket_, 1);
        mc2ipc::write(socket_, text.size());
        mc2ipc::transfer(socket_, const_cast<char *>(text.data()), text.size(), true);
        auto values = response(512);
        std::array<float, 768> out{};
        std::copy(values.begin(), values.end(), out.begin());
        return out;
    }
};
} // namespace amdva::nvr
