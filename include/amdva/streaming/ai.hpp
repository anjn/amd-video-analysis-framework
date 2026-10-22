#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
namespace amdva::streaming {
struct AiMetric {
    std::string stage, camera;
    unsigned samples{}, pending{};
    double mean_ms{}, p50_ms{}, p95_ms{}, max_ms{}, wait_ms{}, rate{}, mean_batch{};
    uint64_t dropped{};
    bool truncated{};
};
// Completed wall-clock measurements. These are not GPU utilization counters.
class AiRuntime {
  public:
    using Clock = std::chrono::steady_clock;
    enum Priority { Tracking, Query, Embedding };
    class Lease {
        AiRuntime *owner_{};
        Priority priority_{};
        Clock::time_point started_;

      public:
        Lease(AiRuntime *, Priority);
        ~Lease();
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
    };
    Lease acquire(Priority p) { return Lease(this, p); }
    void record(const std::string &stage, const std::string &camera, double ms, double wait = 0,
                unsigned batch = 1);
    void queue(const std::string &stage, const std::string &camera, unsigned pending,
               uint64_t drops = 0);
    std::vector<AiMetric> snapshot() const;
    void configure(unsigned fps, unsigned interval_ms);
    bool embedding_ready() const;
    unsigned fps() const;
    unsigned interval_ms() const;
    void effective_interval(unsigned ms);
    unsigned effective_interval() const;

  private:
    struct Sample {
        Clock::time_point at;
        double ms, wait;
        unsigned batch;
    };
    struct Row {
        std::deque<Sample> samples;
        unsigned pending{};
        uint64_t dropped{};
        bool truncated{};
    };
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, Row> rows_;
    Clock::time_point started_ = Clock::now();
    unsigned fps_{2}, interval_{5000}, effective_{5000};
    mutable std::mutex gate_;
    std::condition_variable cv_;
    unsigned waiting_[3]{};
    uint64_t next_ticket_[3]{}, serving_ticket_[3]{};
    bool busy_{};
    Clock::time_point low_due_{};
};
} // namespace amdva::streaming
