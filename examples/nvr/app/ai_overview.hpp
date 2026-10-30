#pragma once
#include "backend/ai.hpp"
#include "backend/config.hpp"
#include <QStringList>
#include <QWidget>
#include <array>
#include <deque>
#include <map>
namespace amdva::nvr {
// Demo summary of the AI Statistics page, derived from the existing measurements only.
struct AiOverviewSample {
    enum State { Idle, Good, Warning, Error };
    struct Decode {
        State state{Idle};
        unsigned cameras{}, live{};
        double fps{}, mpixels{}; // Decoded frames and megapixels per second, all cameras.
        double p99_ms{-1};       // Worst camera's input → decode P99; -1 when unmeasured.
        uint64_t stops{}, gaps{};
        QStringList formats; // For example "H265 1920×1080 ×4".
    } decode;
    struct Gpu {
        State state{Idle};
        double yolo_images{}, yolo_ms{}, yolo_batch{}, reid_crops{}, embedding_images{};
    } gpu;
    struct Npu {
        State state{Idle};
        bool configured{};
        double images{}, mean_ms{}, p95_ms{};
    } npu;
    struct Tracking {
        State state{Idle};
        bool enabled{};
        unsigned people{}, identities{};
        double rate{}, age_ms{-1}; // Association updates/s; oldest per-camera frame age.
        std::vector<std::pair<QString, unsigned>> cameras;
    } tracking;
};
AiOverviewSample summarize_overview(const std::vector<Status> &,
                                    const std::vector<TelemetrySnapshot> &,
                                    const std::vector<AiMetric> &,
                                    const std::map<std::string, PersonTrackFrame> &,
                                    const Options &, bool fatal, Clock::time_point now);
// Four large cards with a 60 second trend each: decode, GPU AI, NPU AI and person tracking.
class AiOverview final : public QWidget {
  public:
    static constexpr size_t history_limit = 60;
    explicit AiOverview(QWidget *parent = nullptr);
    void push(const AiOverviewSample &);
    size_t history() const { return history_[0].size(); }

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    AiOverviewSample sample_;
    std::array<std::deque<double>, 4> history_;
};
} // namespace amdva::nvr
