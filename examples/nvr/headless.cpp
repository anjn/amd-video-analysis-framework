#include "backend/config.hpp"
#include <atomic>
#include <csignal>
#include <iostream>
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
} // namespace
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: amdva-streams CONFIG.json [SECONDS]\n";
        return 1;
    }
    try {
        amdva::streaming::initialize_runtime();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        auto options = amdva::nvr::load_config(argv[1]);
        options.preview_enabled = false;
        amdva::nvr::Controller controller(options);
        const auto start = std::chrono::steady_clock::now();
        const unsigned duration = argc == 3 ? std::stoul(argv[2]) : 0;
        while (!stopping && (!duration || std::chrono::steady_clock::now() - start <
                                              std::chrono::seconds(duration))) {
            if (!controller.fatal().empty())
                throw std::runtime_error(controller.fatal());
            const auto statuses = controller.status();
            unsigned live = 0, recording = 0;
            uint64_t ai = 0;
            for (const auto &status : statuses) {
                live += status.connection == "live";
                recording += status.recording == "recording";
                ai += status.ai + status.background_ai;
            }
            const auto publication = controller.publication_status();
            std::cout << "{\"live\":" << live << ",\"recording\":" << recording << ",\"ai\":" << ai
                      << ",\"results_delivered\":" << publication.delivered
                      << ",\"results_dropped\":" << publication.dropped << ",\"telemetry\":[";
            bool first = true;
            for (const auto &snapshot : controller.telemetry()) {
                if (!first)
                    std::cout << ",";
                first = false;
                std::cout << snapshot.json();
            }
            std::cout << "]}\n" << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        controller.shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
