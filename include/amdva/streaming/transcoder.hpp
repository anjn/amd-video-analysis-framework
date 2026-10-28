#pragma once
#include "amdva/streaming/recorder.hpp"
#include <gst/gst.h>
#include <gst/va/gstvadisplay.h>
#include <functional>
#include <map>
#include <string>

namespace amdva::streaming {
enum class RateControl { CBR, VBR, QVBR };

// Re-encodes one camera's compressed H.264/H.265 stream on the GPU: VA decode,
// optional VA scaling, then `amdencode` (VA H.264, H.265 or AV1 encoder). Frames stay
// in VAMemory; only compressed access units cross the CPU. PTS pass through
// unchanged, so the output can feed a SegmentRecorder that keeps the source timing.
// With Options::frames the decoded frames are also offered on frames(), so a display
// or analysis consumer can share the decode instead of decoding the stream again.
//
// Feed access units with push(), starting at a keyframe, and drain pull() from the
// same thread, for example once per control tick. All methods must be called from
// one thread.
class Transcoder {
  public:
    struct Options {
        RecordCodec codec{RecordCodec::H265};
        RateControl rate_control{RateControl::VBR};
        unsigned bitrate_kbps{2000};
        // 0 or >= the input height keeps the size; the width keeps the aspect. AV1
        // output snaps to a width multiple of 64 and a height multiple of 16.
        unsigned height{};
        unsigned key_interval_frames{60};
        uint64_t queue_limit_bytes{16ull << 20}; // push() refuses beyond this backlog
        // Adds an appsink of decoded NV12 VAMemory frames ("frames", max-buffers=1,
        // drop=true) beside the encoder. Its callbacks run on the decoder thread.
        bool frames{};
        // Called with each input buffer copy before it enters the pipeline, for example
        // to attach timing metadata.
        std::function<void(GstBuffer *)> on_input;
    };
    // `caps` describe the compressed input. Throws std::runtime_error when the input
    // codec is unsupported or the pipeline cannot start.
    Transcoder(GstVaDisplay *display, Options options, GstCaps *caps);
    ~Transcoder();
    Transcoder(const Transcoder &) = delete;
    Transcoder &operator=(const Transcoder &) = delete;

    // Queues one access unit. Returns false when its PTS is invalid or the backlog
    // is over the limit; the caller should restart from a keyframe.
    bool push(GstSample *sample);
    // Next encoded access unit, or nullptr when none is ready. The caller unrefs it.
    GstSample *pull();
    // Pumps messages. Returns false after a pipeline error; see error().
    bool poll();
    std::string error() const { return error_; }
    // Moving average of the time from push() to pull() of the same PTS.
    double latency_ms() const { return latency_ms_; }
    // Access units pulled so far. Fewer than pushed when the video engine fell behind
    // and frames were dropped before the encoder.
    uint64_t encoded() const { return encoded_; }
    const Options &options() const { return options_; }
    // Borrowed elements: the VA decoder, and the decoded-frame appsink (null without
    // Options::frames). Configure the appsink before the first push().
    GstElement *decoder() const { return decoder_; }
    GstElement *frames() const { return frames_; }

  private:
    void release();
    Options options_;
    GstElement *pipeline_{};
    GstElement *input_{};
    GstElement *output_{};
    GstElement *decoder_{};
    GstElement *frames_{};
    std::map<GstClockTime, gint64> pushed_;
    double latency_ms_{};
    uint64_t encoded_{};
    std::string error_;
};
} // namespace amdva::streaming
