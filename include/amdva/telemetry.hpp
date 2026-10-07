#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace amdva {
enum class TimingStage : unsigned {
    Input,
    AccessUnit,
    DecodeInput,
    DecodeOutput,
    PreviewReady,
    Draw,
    Swap,
    Count
};
constexpr unsigned timing_stage_count = unsigned(TimingStage::Count);
const char *timing_stage_name(TimingStage);
struct TelemetryOptions {
    bool enabled{true};
    unsigned stall_ms{1000};
};
struct FrameTrace {
    uint64_t generation{}, sequence{};
    bool background{};
    std::array<int64_t, timing_stage_count> ns{};
};
struct TimingDistribution {
    size_t samples{};
    double mean_ms{}, p95_ms{}, p99_ms{}, maximum_ms{}, effective_window_ms{};
    bool truncated{};
};
struct TimingStageStatus {
    bool supported{}, active{}, stalled{};
    uint64_t count{}, stops{};
    double fps{}, age_ms{-1}, stopped_ms{}, longest_stop_ms{};
    TimingDistribution intervals;
};
struct TelemetrySnapshot {
    std::string source;
    uint64_t generation{};
    bool enabled{};
    std::array<std::array<TimingStageStatus, timing_stage_count>, 2> stages;
    // input_decode, decoder, input_draw, input_swap, decode_preview, preview_draw, draw_swap
    std::array<std::array<TimingDistribution, 7>, 2> latency;
    uint64_t encoded_discarded{}, latest_replaced{}, preview_discarded{}, unmatched_rtp{},
        ambiguous_pts{}, unmatched_decode{}, expired{}, capacity_evicted{}, stale{}, invalid_time{},
        submitted_without_output{}, presentation_unobserved{};
    std::string json() const;
};
enum class TimingCounter {
    EncodedDiscarded,
    LatestReplaced,
    PreviewDiscarded,
    UnmatchedRtp,
    AmbiguousPts,
    UnmatchedDecode,
    Expired,
    CapacityEvicted,
    Stale,
    InvalidTime,
    SubmittedWithoutOutput,
    PresentationUnobserved
};
class Telemetry {
  public:
    using Now = std::function<int64_t()>;
    explicit Telemetry(std::string source, TelemetryOptions = {}, Now = {});
    ~Telemetry();
    static int64_t monotonic_ns();
    int64_t now() const;
    bool enabled() const;
    void begin_generation(uint64_t = 0);
    uint64_t generation() const;
    FrameTrace frame(int64_t input_ns = 0, bool background = false);
    // Returns true only for the first observation of this frame/stage.
    bool mark(FrameTrace &, TimingStage, int64_t at_ns = 0);
    void availability(TimingStage, bool supported, bool active);
    void activity(TimingStage, bool active);
    void counter(TimingCounter, uint64_t amount = 1);
    TelemetrySnapshot snapshot();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct TraceToken {
    std::shared_ptr<Telemetry> telemetry;
    FrameTrace frame;
    bool mark(TimingStage stage, int64_t at_ns = 0) {
        return telemetry && telemetry->mark(frame, stage, at_ns);
    }
};
} // namespace amdva
