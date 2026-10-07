#include "amdva/telemetry.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
namespace amdva {
namespace {
constexpr int64_t window_ns = 10000000000LL, retention_ns = 30000000000LL;
constexpr size_t capacity = 4096;
constexpr const char *stages[] = {"input",         "access_unit", "decode_input", "decode_output",
                                  "preview_ready", "draw",        "swap"};
constexpr const char *latencies[] = {"input_decode",   "decoder",      "input_draw", "input_swap",
                                     "decode_preview", "preview_draw", "draw_swap"};
constexpr std::pair<unsigned, unsigned> edges[] = {{0, 3}, {2, 3}, {0, 5}, {0, 6},
                                                   {3, 4}, {4, 5}, {5, 6}};
struct Samples {
    std::deque<std::pair<int64_t, int64_t>> values;
    int64_t last_eviction{};
    void prune(int64_t now) {
        while (!values.empty() && values.front().first < now - window_ns)
            values.pop_front();
    }
    void add(int64_t at, int64_t value) {
        prune(at);
        if (values.size() == capacity) {
            last_eviction = values.front().first;
            values.pop_front();
        }
        values.emplace_back(at, value);
    }
    TimingDistribution stats(int64_t now) {
        prune(now);
        TimingDistribution result;
        result.samples = values.size();
        result.truncated = last_eviction && last_eviction >= now - window_ns;
        if (values.empty())
            return result;
        result.effective_window_ms = double(now - values.front().first) / 1e6;
        std::vector<int64_t> sorted;
        sorted.reserve(values.size());
        long double sum = 0;
        for (auto v : values) {
            sorted.push_back(v.second);
            sum += v.second;
        }
        std::sort(sorted.begin(), sorted.end());
        result.mean_ms = double(sum / sorted.size()) / 1e6;
        result.p95_ms = double(sorted[size_t(std::ceil(sorted.size() * .95)) - 1]) / 1e6;
        result.p99_ms = double(sorted[size_t(std::ceil(sorted.size() * .99)) - 1]) / 1e6;
        result.maximum_ms = double(sorted.back()) / 1e6;
        return result;
    }
};
struct Stage {
    TimingStageStatus status;
    Samples intervals, arrivals;
    int64_t last{}, stop_started{}, total_stop{}, longest_stop{}, active_since{};
    void update(int64_t at, int64_t threshold, bool monitor) {
        if (!monitor || !status.active || !last)
            return;
        if (at - last >= threshold && !stop_started) {
            stop_started = last;
            status.stops++;
        }
    }
    void close_stop(int64_t at) {
        if (stop_started) {
            auto d = std::max<int64_t>(0, at - stop_started);
            total_stop += d;
            longest_stop = std::max(longest_stop, d);
            stop_started = 0;
        }
    }
};
std::string quote(const std::string &value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\')
            out << '\\' << char(c);
        else if (c < 32)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c)
                << std::dec;
        else
            out << char(c);
    }
    out << '"';
    return out.str();
}
void distribution(std::ostream &out, const TimingDistribution &s) {
    out << "{\"samples\":" << s.samples << ",\"truncated\":" << (s.truncated ? "true" : "false")
        << ",\"effective_window_ms\":" << s.effective_window_ms;
    for (auto p : {std::pair<const char *, double>{"mean_ms", s.mean_ms},
                   {"p95_ms", s.p95_ms},
                   {"p99_ms", s.p99_ms},
                   {"maximum_ms", s.maximum_ms}}) {
        out << ",\"" << p.first << "\":";
        if (s.samples)
            out << p.second;
        else
            out << "null";
    }
    out << '}';
}
} // namespace
const char *timing_stage_name(TimingStage s) {
    return unsigned(s) < timing_stage_count ? stages[unsigned(s)] : "unknown";
}
struct Telemetry::Impl {
    std::string source;
    TelemetryOptions options;
    Now clock;
    std::mutex mutex;
    uint64_t generation{}, sequence{};
    int64_t started{};
    struct Seen {
        int64_t at{};
        unsigned mask{};
        std::array<int64_t, timing_stage_count> ns{};
    };
    std::map<uint64_t, Seen> seen;
    std::array<std::array<Stage, timing_stage_count>, 2> stage;
    std::array<std::array<Samples, 7>, 2> latency;
    std::array<uint64_t, 12> counters{};
    Impl(std::string id, TelemetryOptions o, Now n)
        : source(std::move(id)), options(o), clock(std::move(n)) {
        if (!options.stall_ms)
            throw std::invalid_argument("Telemetry stall_ms must be positive");
        if (!clock)
            clock = Telemetry::monotonic_ns;
        started = clock();
    }
};
Telemetry::Telemetry(std::string id, TelemetryOptions options, Now now)
    : impl_(std::make_unique<Impl>(std::move(id), options, std::move(now))) {}
Telemetry::~Telemetry() = default;
int64_t Telemetry::monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
int64_t Telemetry::now() const { return impl_->clock(); }
bool Telemetry::enabled() const { return impl_->options.enabled; }
void Telemetry::begin_generation(uint64_t generation) {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.generation = std::max(generation, s.generation + 1);
    s.sequence = 0;
    s.seen.clear();
    s.started = now();
    s.latency = {};
    for (auto &group : s.stage)
        for (auto &v : group) {
            v.close_stop(s.started);
            v.last = 0;
            v.active_since = s.started;
            v.intervals = {};
            v.arrivals = {};
        }
}
uint64_t Telemetry::generation() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->generation;
}
void Telemetry::activity(TimingStage stage, bool active) {
    if (unsigned(stage) >= timing_stage_count)
        return;
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    for (auto &group : s.stage) {
        auto &v = group[unsigned(stage)];
        if (v.status.active != active) {
            v.close_stop(now());
            v.last = 0;
            v.active_since = now();
            v.intervals = {};
            v.arrivals = {};
        }
        v.status.active = v.status.supported && active;
    }
}
FrameTrace Telemetry::frame(int64_t input_ns, bool background) {
    FrameTrace frame;
    if (!enabled())
        return frame;
    {
        auto &s = *impl_;
        std::lock_guard<std::mutex> lock(s.mutex);
        frame.generation = s.generation;
        frame.sequence = ++s.sequence;
        frame.background = background;
    }
    if (input_ns)
        mark(frame, TimingStage::Input, input_ns);
    return frame;
}
bool Telemetry::mark(FrameTrace &frame, TimingStage which, int64_t at) {
    auto &s = *impl_;
    if (!enabled() || unsigned(which) >= timing_stage_count || !frame.sequence)
        return false;
    if (!at)
        at = now();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (frame.generation != s.generation) {
        s.counters[unsigned(TimingCounter::Stale)]++;
        return false;
    }
    if (at <= 0) {
        s.counters[unsigned(TimingCounter::InvalidTime)]++;
        return false;
    }
    // Duplicate notifications are normal during repaint; ignore before ordering checks.
    const unsigned index = unsigned(which);
    auto previous = s.seen.find(frame.sequence);
    if (previous != s.seen.end())
        for (unsigned i = 0; i < timing_stage_count; i++)
            if (!frame.ns[i])
                frame.ns[i] = previous->second.ns[i];
    if (frame.ns[index] || (previous != s.seen.end() && (previous->second.mask & (1u << index))))
        return false;
    // Stage timestamps must be ordered; B-frame output order need not match input order.
    for (unsigned i = 0; i < index; i++)
        if (frame.ns[i] > at) {
            s.counters[unsigned(TimingCounter::InvalidTime)]++;
            return false;
        }
    for (unsigned i = index + 1; i < timing_stage_count; i++)
        if (frame.ns[i] && frame.ns[i] < at) {
            s.counters[unsigned(TimingCounter::InvalidTime)]++;
            return false;
        }
    for (auto it = s.seen.begin(); it != s.seen.end();) {
        if (at - it->second.at > retention_ns)
            it = s.seen.erase(it);
        else
            break;
    }
    auto found = s.seen.find(frame.sequence);
    if (found == s.seen.end()) {
        if (s.seen.size() == capacity) {
            s.seen.erase(s.seen.begin());
            s.counters[unsigned(TimingCounter::CapacityEvicted)]++;
        }
        found = s.seen.emplace(frame.sequence, Impl::Seen{at, 0}).first;
    }
    if (frame.ns[index] || (found->second.mask & (1u << index)))
        return false;
    frame.ns[index] = at;
    found->second.mask |= 1u << index;
    found->second.ns = frame.ns;
    const unsigned group = frame.background && index >= unsigned(TimingStage::DecodeInput) ? 1 : 0;
    auto &v = s.stage[group][index];
    v.status.supported = true;
    v.update(at, int64_t(s.options.stall_ms) * 1000000, group == 0);
    v.close_stop(at);
    if (v.last && at >= v.last)
        v.intervals.add(at, at - v.last);
    // RTP observations can arrive late in AU order. Do not move the stage clock backwards.
    if (at >= v.last) {
        v.last = at;
        v.arrivals.add(at, 0);
    }
    v.status.count++;
    auto &latency = s.latency[group];
    for (unsigned e = 0; e < 7; e++)
        if (edges[e].second == index && frame.ns[edges[e].first])
            latency[e].add(at, at - frame.ns[edges[e].first]);
    return true;
}
void Telemetry::availability(TimingStage stage, bool supported, bool active) {
    if (unsigned(stage) >= timing_stage_count)
        return;
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    for (auto &group : s.stage) {
        auto &v = group[unsigned(stage)];
        if (v.status.active != active) {
            v.close_stop(now());
            v.last = 0;
            v.active_since = now();
            v.intervals = {};
            v.arrivals = {};
        }
        v.status.supported = supported;
        v.status.active = supported && active;
    }
}
void Telemetry::counter(TimingCounter counter, uint64_t amount) {
    if (!enabled() || unsigned(counter) >= 12)
        return;
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.counters[unsigned(counter)] += amount;
}
TelemetrySnapshot Telemetry::snapshot() {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    TelemetrySnapshot out;
    out.source = s.source;
    out.generation = s.generation;
    out.enabled = enabled();
    const auto at = now();
    for (unsigned g = 0; g < 2; g++) {
        for (unsigned i = 0; i < timing_stage_count; i++) {
            auto &v = s.stage[g][i];
            v.update(at, int64_t(s.options.stall_ms) * 1000000, g == 0);
            auto &d = out.stages[g][i];
            d = v.status;
            d.intervals = v.intervals.stats(at);
            v.arrivals.prune(at);
            d.fps = double(v.arrivals.values.size()) /
                    (double(std::max<int64_t>(
                         1000000,
                         std::min(window_ns, at - (v.active_since ? v.active_since : s.started)))) /
                     1e9);
            d.age_ms = v.last ? double(at - v.last) / 1e6 : -1;
            d.stalled = v.stop_started != 0;
            const auto current = v.stop_started ? std::max<int64_t>(0, at - v.stop_started) : 0;
            d.stopped_ms = double(v.total_stop + current) / 1e6;
            d.longest_stop_ms = double(std::max(v.longest_stop, current)) / 1e6;
        }
        for (unsigned i = 0; i < 7; i++)
            out.latency[g][i] = s.latency[g][i].stats(at);
    }
    out.encoded_discarded = s.counters[0];
    out.latest_replaced = s.counters[1];
    out.preview_discarded = s.counters[2];
    out.unmatched_rtp = s.counters[3];
    out.ambiguous_pts = s.counters[4];
    out.unmatched_decode = s.counters[5];
    out.expired = s.counters[6];
    out.capacity_evicted = s.counters[7];
    out.stale = s.counters[8];
    out.invalid_time = s.counters[9];
    out.submitted_without_output = s.counters[10];
    out.presentation_unobserved = s.counters[11];
    return out;
}
std::string TelemetrySnapshot::json() const {
    std::ostringstream out;
    out << std::setprecision(10) << "{\"schema\":1,\"source\":" << quote(source)
        << ",\"generation\":" << generation << ",\"enabled\":" << (enabled ? "true" : "false");
    for (unsigned g = 0; g < 2; g++) {
        out << ",\"" << (g ? "background" : "continuous") << "\":{\"stages\":{";
        for (unsigned i = 0; i < timing_stage_count; i++) {
            if (i)
                out << ',';
            const auto &v = stages[g][i];
            out << quote(::amdva::stages[i])
                << ":{\"supported\":" << (v.supported ? "true" : "false")
                << ",\"active\":" << (v.active ? "true" : "false") << ",\"count\":" << v.count
                << ",\"fps\":" << v.fps << ",\"age_ms\":";
            if (v.age_ms < 0)
                out << "null";
            else
                out << v.age_ms;
            out << ",\"stalled\":" << (v.stalled ? "true" : "false") << ",\"stops\":" << v.stops
                << ",\"stopped_ms\":" << v.stopped_ms
                << ",\"longest_stop_ms\":" << v.longest_stop_ms << ",\"interval\":";
            distribution(out, v.intervals);
            out << '}';
        }
        out << "},\"latency\":{";
        for (unsigned i = 0; i < 7; i++) {
            if (i)
                out << ',';
            out << quote(latencies[i]) << ':';
            distribution(out, latency[g][i]);
        }
        out << "}}";
    }
    out << ",\"counters\":{";
    const std::pair<const char *, uint64_t> counters[] = {
        {"encoded_discarded", encoded_discarded},
        {"latest_replaced", latest_replaced},
        {"preview_discarded", preview_discarded},
        {"unmatched_rtp", unmatched_rtp},
        {"ambiguous_pts", ambiguous_pts},
        {"unmatched_decode", unmatched_decode},
        {"expired", expired},
        {"capacity_evicted", capacity_evicted},
        {"stale", stale},
        {"invalid_time", invalid_time},
        {"submitted_without_output", submitted_without_output},
        {"presentation_unobserved", presentation_unobserved}};
    for (unsigned i = 0; i < 12; i++) {
        if (i)
            out << ',';
        out << quote(counters[i].first) << ':' << counters[i].second;
    }
    out << "}}";
    return out.str();
}
} // namespace amdva
