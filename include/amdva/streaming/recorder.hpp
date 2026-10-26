#pragma once
#include "amdva/streaming/catalog.hpp"
#include <atomic>
#include <functional>
#include <gst/gst.h>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace amdva::streaming {
enum class RecordCodec { H264, H265 };

// Records one camera's compressed H.264/H.265 stream into rolling Matroska
// segments (`splitmuxsink`) and keeps the Catalog in step: every segment is
// registered when it opens, timed from the recorder's media clock, closed with its
// duration, and marked interrupted if the process stops before it is finalized. The
// stream is never decoded or re-encoded.
//
// Feed access units with push(), starting at a keyframe; call poll() periodically
// (for example once per control tick) to pump the pipeline bus. After end() the
// recorder finalizes the open segment and poll() returns true when it is done.
// All methods except camera_name() must be called from one thread.
class SegmentRecorder {
  public:
    struct Options {
        std::string channel;
        std::string camera_name;
        std::string session;                        // empty: a random UUID
        RecordCodec codec{RecordCodec::H264};
        unsigned segment_seconds{60};
        uint64_t queue_limit_bytes{16ull << 20};    // push() refuses beyond this backlog
        std::function<void()> on_destroyed;         // e.g. schedule storage reclamation
    };
    // `caps` are the upstream caps of the compressed stream (byte-stream or avc/hvc1).
    // Throws std::runtime_error when the recorder pipeline cannot start.
    SegmentRecorder(Catalog &catalog, Options options, GstCaps *caps);
    ~SegmentRecorder();
    SegmentRecorder(const SegmentRecorder &) = delete;
    SegmentRecorder &operator=(const SegmentRecorder &) = delete;

    // Queues one access unit. Timestamps are rebased to the first accepted PTS.
    // Returns false when the sample was not queued (invalid or earlier PTS, or the
    // backlog is over the limit); the caller should end() and wait for a keyframe.
    bool push(GstSample *sample);
    void end();
    // Pumps messages. Returns true once the recorder has finished (EOS, error or the
    // five second finalize deadline); open segments are then closed or interrupted.
    bool poll();
    bool ending() const { return ending_; }
    // True once after a failure (pipeline error or a segment that could not be
    // registered); error() describes the last pipeline error.
    bool consume_failure() { return failed_.exchange(false); }
    std::string error() const;
    void camera_name(const std::string &name);
    const std::string &session() const { return options_.session; }
    // Wall-clock time (ms) of a source PTS, once the first sample was accepted.
    std::optional<int64_t> wall_time_ms(GstClockTime original_pts) const;
    // Names of segments opened and not yet closed.
    std::vector<std::string> open_segments() const;

  private:
    static gchar *location(GstElement *, guint fragment, GstSample *first, gpointer self);
    Catalog &catalog_;
    Options options_;
    GstElement *pipeline_{};
    GstElement *input_{};
    GstClockTime origin_{GST_CLOCK_TIME_NONE};
    int64_t wall_origin_ms_{-1};
    bool ending_{};
    std::chrono::steady_clock::time_point deadline_;
    mutable std::mutex mutex_;
    std::vector<std::string> files_;
    std::string error_;
    std::atomic<bool> failed_{false};
};
} // namespace amdva::streaming
