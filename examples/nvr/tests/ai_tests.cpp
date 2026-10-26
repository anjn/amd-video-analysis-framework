#include "backend/ai.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace amdva::nvr;
void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
int main() {
    try {
        AiRuntime r;
        r.configure(5, 15000);
        r.effective_interval(5000);
        require(r.fps() == 5 && r.effective_interval() == 15000,
                "Requested interval is a lower bound");
        bool invalid = false;
        try {
            r.configure(0, 5000);
        } catch (const std::invalid_argument &) {
            invalid = true;
        }
        require(invalid && r.fps() == 5, "Invalid configuration is atomic");
        r.configure(.3, 700);
        r.effective_interval(800);
        require(r.fps() == .3 && r.interval_ms() == 700 && r.effective_interval() == 800,
                "Fractional Hz and subsecond intervals");
        r.record("inference", "cam0", 2, 3, 4);
        r.record("inference", "cam0", 4, 5, 2);
        r.record("inference", "cam0", std::numeric_limits<double>::quiet_NaN());
        r.queue("inference", "cam0", 8, 2);
        r.queue("inference", "cam0", 7, 1);
        auto m = r.snapshot().back();
        require(r.snapshot().front().camera.empty() && r.snapshot().front().mean_ms == 3,
                "Cross-camera summary");
        require(m.samples == 2 && m.mean_ms == 3 && m.wait_ms == 4 && m.mean_batch == 3,
                "Timing aggregation");
        require(m.pending == 7 && m.dropped == 3 && m.max_ms == 4, "Queue counters");
        for (int i = 0; i < 9000; ++i)
            r.record("inference", "cam0", 1);
        require(r.snapshot().front().samples == 8192 && r.snapshot().front().truncated,
                "Bounded statistics");
        r.mobileclip_budget(35, 100);
        require(r.mobileclip_budget() == std::pair<unsigned, unsigned>{35, 100},
                "Per-device budget configuration");
        bool bad_budget = false;
        try {
            r.mobileclip_budget(0, 100);
        } catch (const std::invalid_argument &) {
            bad_budget = true;
        }
        require(bad_budget && r.mobileclip_budget().first == 35, "Invalid budgets are atomic");
        AiRuntime gate;
        {
            auto lease = gate.acquire(AiRuntime::Embedding);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(!gate.embedding_ready() && gate.embedding_ready(true),
                "MobileCLIP bypasses Gemma cooldown");
        {
            auto lease = gate.acquire(AiRuntime::Tracking);
            require(!gate.embedding_ready(true), "MobileCLIP still yields to tracking");
        }
        std::atomic<int> inside{0}, overlap{0};
        auto run = [&](AiRuntime::Priority p) {
            for (int i = 0; i < 20; ++i) {
                auto lease = r.acquire(p);
                if (inside.fetch_add(1))
                    ++overlap;
                std::this_thread::yield();
                --inside;
            }
        };
        auto a = std::async(std::launch::async, run, AiRuntime::Tracking);
        auto b = std::async(std::launch::async, run, AiRuntime::Query);
        auto c = std::async(std::launch::async, run, AiRuntime::Embedding);
        auto d = std::async(std::launch::async, run, AiRuntime::MobileClip);
        a.get();
        b.get();
        c.get();
        d.get();
        require(overlap == 0, "GPU stages are serialized");
        std::cout << "AI statistics and execution gate passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
