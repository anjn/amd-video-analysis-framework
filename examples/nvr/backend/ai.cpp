#include "backend/ai.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace amdva::nvr {
AiRuntime::Lease::Lease(AiRuntime *owner, Priority p) : owner_(owner), priority_(p) {
    auto &o = *owner;
    std::unique_lock<std::mutex> lock(o.gate_);
    ++o.waiting_[p];
    const auto ticket = o.next_ticket_[p]++;
    for (;;) {
        bool earlier = false;
        for (int i = 0; i < p; ++i)
            earlier |= o.waiting_[i] != 0;
        if (!o.busy_ && !earlier && ticket == o.serving_ticket_[p] &&
            (p != Embedding || Clock::now() >= o.low_due_))
            break;
        if (p == Embedding && o.low_due_ > Clock::now())
            o.cv_.wait_until(lock, o.low_due_);
        else
            o.cv_.wait(lock);
    }
    --o.waiting_[p];
    ++o.serving_ticket_[p];
    o.busy_ = true;
    started_ = Clock::now();
}
AiRuntime::Lease::~Lease() {
    if (!owner_)
        return;
    auto &o = *owner_;
    std::lock_guard<std::mutex> lock(o.gate_);
    if (priority_ == Embedding)
        o.low_due_ = Clock::now() + (Clock::now() - started_) * 4;
    o.busy_ = false;
    o.cv_.notify_all();
}
void AiRuntime::configure(double fps, unsigned interval) {
    if ((!std::isfinite(fps) || fps < .1 || fps > 10) || interval < 100 || interval > 60000)
        throw std::invalid_argument("Invalid AI frequency");
    std::lock_guard<std::mutex> lock(mutex_);
    fps_ = fps;
    interval_ = interval;
    effective_ = std::max(effective_, interval);
}
bool AiRuntime::embedding_ready(bool mobileclip) const {
    std::lock_guard<std::mutex> l(gate_);
    return !busy_ && !waiting_[Tracking] && !waiting_[Query] &&
           (mobileclip || Clock::now() >= low_due_);
}
void AiRuntime::mobileclip_budget(unsigned gpu, unsigned npu) {
    if (!gpu || gpu > 100 || !npu || npu > 100)
        throw std::invalid_argument("MobileCLIP budgets must be 1 to 100 percent");
    std::lock_guard<std::mutex> lock(mutex_);
    mobile_gpu_ = gpu;
    mobile_npu_ = npu;
}
std::pair<unsigned, unsigned> AiRuntime::mobileclip_budget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {mobile_gpu_, mobile_npu_};
}
double AiRuntime::fps() const {
    std::lock_guard<std::mutex> l(mutex_);
    return fps_;
}
unsigned AiRuntime::interval_ms() const {
    std::lock_guard<std::mutex> l(mutex_);
    return interval_;
}
void AiRuntime::effective_interval(unsigned ms) {
    std::lock_guard<std::mutex> l(mutex_);
    effective_ = std::clamp(ms, interval_, 60000u);
}
unsigned AiRuntime::effective_interval() const {
    std::lock_guard<std::mutex> l(mutex_);
    return effective_;
}
void AiRuntime::record(const std::string &stage, const std::string &camera, double ms, double wait,
                       unsigned batch) {
    if (!std::isfinite(ms) || ms < 0 || !std::isfinite(wait) || wait < 0)
        return;
    if (!camera.empty())
        record(stage, "", ms, wait, batch);
    std::lock_guard<std::mutex> l(mutex_);
    auto key = std::make_pair(stage, camera);
    if (!rows_.count(key) && rows_.size() >= 512)
        return;
    auto &r = rows_[key];
    auto now = Clock::now();
    while (!r.samples.empty() && now - r.samples.front().at > std::chrono::seconds(60))
        r.samples.pop_front();
    if (r.samples.size() >= 8192) {
        r.samples.pop_front();
        r.truncated = true;
    }
    r.samples.push_back({now, ms, wait, batch});
}
void AiRuntime::queue(const std::string &s, const std::string &c, unsigned pending,
                      uint64_t drops) {
    std::lock_guard<std::mutex> l(mutex_);
    auto key = std::make_pair(s, c);
    if (!rows_.count(key) && rows_.size() >= 512)
        return;
    auto &r = rows_[key];
    r.pending = pending;
    r.dropped += drops;
}
std::vector<AiMetric> AiRuntime::snapshot() const {
    std::lock_guard<std::mutex> l(mutex_);
    std::vector<AiMetric> out;
    auto now = Clock::now();
    double window =
        std::max(.001, std::min(60., std::chrono::duration<double>(now - started_).count()));
    for (const auto &[key, r] : rows_) {
        AiMetric m;
        m.stage = key.first;
        m.camera = key.second;
        m.pending = r.pending;
        m.dropped = r.dropped;
        m.truncated = r.truncated;
        std::vector<double> v;
        for (const auto &s : r.samples)
            if (now - s.at <= std::chrono::seconds(60)) {
                v.push_back(s.ms);
                m.mean_ms += s.ms;
                m.wait_ms += s.wait;
                m.mean_batch += s.batch;
            }
        m.samples = v.size();
        if (!v.empty()) {
            std::sort(v.begin(), v.end());
            m.mean_ms /= v.size();
            m.wait_ms /= v.size();
            m.mean_batch /= v.size();
            m.p50_ms = v[v.size() / 2];
            m.p95_ms = v[std::min(v.size() - 1, size_t(v.size() * .95))];
            m.max_ms = v.back();
            m.rate = v.size() / window;
        }
        out.push_back(m);
    }
    return out;
}
} // namespace amdva::nvr
