#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <gst/gst.h>
#include <memory>
#include <mutex>
#include <string>

typedef struct _GstAppSink GstAppSink; // from <gst/app/gstappsink.h>

namespace amdva::streaming {
using Sample = std::shared_ptr<GstSample>;

// Receives one RTSP camera as compressed access units. The video stream is
// depayloaded and parsed to byte-stream, AU-aligned H.264/H.265 and queued without
// decoding. The queue is bounded: when it exceeds its byte budget or spans more
// than two seconds of PTS it is discarded as a whole and take_overflow() reports
// it, so a stalled consumer resynchronizes at the next keyframe instead of growing.
//
// Typical use from a control loop: start(), then every tick poll() for failures,
// take() the queued samples, and stop() on error (reconnect policy is the caller's).
class RtspIngest {
  public:
    struct Options {
        std::string uri;
        unsigned latency_ms{100};
        uint64_t queue_limit_bytes{16ull << 20};
        unsigned timeout_seconds{5};              // no access unit for this long is a failure
    };
    // Optional observers, called on GStreamer threads.
    struct Hooks {
        std::function<void(GstElement *rtspsrc)> on_source;           // after rtspsrc creation
        std::function<void(GstPad *sink)> on_access_unit_pad;         // the byte-stream appsink
        std::function<void(uint64_t count)> on_discarded;             // queued AUs dropped
    };
    explicit RtspIngest(Options options, Hooks hooks = {});
    ~RtspIngest();
    RtspIngest(const RtspIngest &) = delete;
    RtspIngest &operator=(const RtspIngest &) = delete;

    // Builds and starts the pipeline. Throws std::runtime_error on failure.
    void start();
    // Stops the pipeline and discards queued samples. Safe to call when not running.
    void stop();
    bool running() const { return pipeline_ != nullptr; }
    // Pumps the bus and checks the timeout. Returns an error description, or an empty
    // string while the stream is healthy. Raw plugin text may contain the camera URI
    // and credentials: sanitize before showing it.
    std::string poll();
    // Moves out the queued access units, oldest first.
    std::deque<Sample> take();
    // True once after the queue was discarded.
    bool take_overflow() { return overflow_.exchange(false); }
    // 0 until the video stream is seen, 1 H.264, 2 H.265, -1 unsupported.
    int codec() const { return codec_.load(); }
    // Access units and bytes received since start(), counted before any queue drop.
    uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }
    uint64_t bytes() const { return bytes_.load(std::memory_order_relaxed); }

  private:
    static GstFlowReturn received(GstAppSink *sink, gpointer self);
    static void pad_added(GstElement *, GstPad *pad, gpointer self);
    Options options_;
    Hooks hooks_;
    GstElement *pipeline_{};
    std::atomic<int> codec_{0};
    std::atomic<bool> overflow_{false};
    std::atomic<uint64_t> frames_{0}, bytes_{0};
    std::atomic<int64_t> last_packet_us_{0};
    std::mutex mutex_;
    std::deque<Sample> queue_;
    uint64_t queued_{};
};
} // namespace amdva::streaming
