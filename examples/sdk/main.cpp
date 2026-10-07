#include <amdva/streaming/config.hpp>
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: headless CONFIG.json\n";
        return 1;
    }
    try {
        amdva::streaming::initialize_runtime();
        auto options = amdva::streaming::load_config(argv[1]);
        options.preview_enabled = false;
        amdva::streaming::Controller service(options);
        // UI, HTTP or IPC controls can call service.add/remove/select; the
        // controller serializes them without exposing pipeline thread details.
        for (unsigned i = 0; i < 30; i++) {
            if (!service.fatal().empty())
                throw std::runtime_error(service.fatal());
            for (const auto &channel : service.status())
                std::cout << channel.source.id << " " << channel.connection << " "
                          << channel.recording << " " << channel.ai << '\n';
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        service.shutdown();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
