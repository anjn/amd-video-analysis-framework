#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
namespace amdva::nvr {
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
    enum Priority { Tracking, Query, Embedding, MobileClip };
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
    void configure(double fps, unsigned interval_ms);
    bool embedding_ready(bool mobileclip = false) const;
    void mobileclip_budget(unsigned gpu, unsigned npu);
    std::pair<unsigned, unsigned> mobileclip_budget() const;
    double fps() const;
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
    double fps_{2};
    unsigned interval_{5000}, effective_{5000};
    unsigned mobile_gpu_{100}, mobile_npu_{100};
    mutable std::mutex gate_;
    std::condition_variable cv_;
    unsigned waiting_[4]{};
    uint64_t next_ticket_[4]{}, serving_ticket_[4]{};
    bool busy_{};
    Clock::time_point low_due_{};
};
} // namespace amdva::nvr
