#pragma once
#include "amdva/gst_telemetry.hpp"
#include "amdva/inference.hpp"
#include "amdva/publishing.hpp"
#include "amdva/runtime.hpp"
#include "amdva/streaming/catalog.hpp"
#include "amdva/tracking.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <gst/gst.h>
#include <gst/va/gstvadisplay.h>
#include <gst/video/video.h>
#include <map>
#include <mutex>
#include <thread>
namespace amdva::streaming {
// Call before creating GStreamer, Qt or worker threads.
void initialize_runtime();
using Clock = std::chrono::steady_clock;
using Sample = std::shared_ptr<GstSample>;
struct Source {
    std::string id, name, uri;
    bool selected{};
};
struct Options {
    std::string model, single_model, model_info, recording_root{"recordings"};
    unsigned batch{4}, segment_seconds{60};
    bool background_ai_enabled{};
    bool tracking_enabled{true};
    bool preview_enabled{true};
    PublicationOptions results;
    TelemetryOptions telemetry;
    unsigned background_ai_interval_seconds{5};
    uint64_t quota{100ull << 30}, reserve{1ull << 30};
    std::vector<Source> sources;
};
struct Status {
    Source source;
    std::string connection{"connecting"}, recording{"waiting"}, error;
    int slot{-1};
    uint64_t received{}, decoded{}, ai{}, gaps{}, generation{};
    double display_fps{}, ai_fps{};
    unsigned ai_age_ms{};
    std::string background_state{"disabled"};
    uint64_t background_decoded{}, background_ai{}, background_attempts{};
    unsigned background_age_ms{};
    bool background_active{};
    std::vector<Detection> background_detections;
    int64_t background_capture_us{};
};
struct Frame {
    TraceToken timing;
    Sample sample;
    uint64_t generation{};
    unsigned width{}, height{};
    Clock::time_point received;
    bool preview_consumed{};
};
struct Plane {
    TraceToken timing;
    VASurfaceID surface{VA_INVALID_SURFACE};
    // 0 free; 1 complete; 2 displayed; 3 fence pending. GL objects belong
    // exclusively to the UI thread; VA writes only to state 0.
    int state{};
    unsigned width{}, height{}, original_width{}, original_height{};
    uint64_t generation{}, sequence{};
    Clock::time_point time;
};
struct Slot {
    std::mutex mutex;
    std::unique_ptr<VaSession> va, preview_va;
    std::array<unsigned, 4> preview_geometry{}, ai_geometry{};
    VASurfaceID ai_surface{VA_INVALID_SURFACE};
    std::unique_ptr<ImportedSurface> image;
    std::array<Plane, 3> preview;
    Frame latest;
    std::string channel;
    uint64_t generation{}, sequence{};
    std::vector<Detection> detections;
    std::vector<TrackedObject> tracked;
    std::unique_ptr<MetadataTracker> tracker;
    uint64_t tracking_generation{}, tracking_observation{};
    Clock::time_point detected_at{}, last_ai{};
    unsigned detection_width{}, detection_height{};
    unsigned target_width{640}, target_height{360};
    uint64_t processed{}, inferred{};
    std::string error;
    bool job_done{};
    ~Slot();
};
class Controller {
  public:
    explicit Controller(Options options);
    ~Controller();
    void add(Source source);
    void remove(const std::string &id);
    void select(const std::string &id, bool selected);
    std::vector<Status> status() const;
    std::array<Slot, 16> &gpu_slots() { return slots_; }
    GstVaDisplay *display() const { return display_; }
    std::string fatal() const;
    bool status_ready() const { return status_ready_.load(); }
    bool inference_ready() const { return inference_ready_.load(); }
    unsigned last_batch() const { return last_batch_.load(); }
    double batch_ms() const { return batch_ms_.load(); }
    PublicationStatus publication_status() const;
    std::vector<TelemetrySnapshot> telemetry() const;
    void presentation_active(bool);
    void shutdown();

  private:
    struct Channel;
    Options options_;
    GstVaDisplay *display_{};
    std::array<Slot, 16> slots_;
    std::array<Slot, 4> background_slots_;
    Slot &ai_slot(unsigned index) {
        return index < 16 ? slots_[index] : background_slots_[index - 16];
    }
    std::atomic<bool> stopping_{false}, initial_enqueued_{false};
    std::atomic<bool> presentation_observed_{true};
    uint64_t next_generation_{0};
    std::thread control_, gpu_, preview_;
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> commands_;
    std::vector<Status> statuses_;
    mutable std::mutex telemetry_mutex_;
    std::map<std::string, std::shared_ptr<Telemetry>> telemetry_;
    std::string fatal_;
    std::vector<std::unique_ptr<Channel>> channels_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<ResultPublisher> publisher_;
    std::atomic<bool> inference_ready_{false}, status_ready_{false};
    std::atomic<unsigned> last_batch_{0};
    std::atomic<double> batch_ms_{0};
    bool reclaim_pending_{false};
    Clock::time_point reclaim_at_{};
    void request_reclaim();
    void command(std::function<void()> f);
    void loop();
    void gpu_loop();
    void preview_loop();
    void assign(Channel &, bool);
};
} // namespace amdva::streaming
