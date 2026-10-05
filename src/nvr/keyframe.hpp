#pragma once
#include <cstdint>
#include <gst/codecparsers/gsth264parser.h>
#include <gst/codecparsers/gsth265parser.h>
#include <gst/gst.h>
#include <map>
#include <vector>
namespace amdva::nvr {
// Control-thread-only Annex B AU inspection; never maps decoded pixels.
class KeyframeCache {
    GstH264NalParser *h264_{gst_h264_nal_parser_new()};
    GstH265Parser *h265_{gst_h265_parser_new()};
    std::map<std::pair<unsigned, unsigned>, std::vector<uint8_t>> parameters_;
    int codec_{};
    int type_{-1};

  public:
    KeyframeCache() = default;
    KeyframeCache(const KeyframeCache &) = delete;
    KeyframeCache &operator=(const KeyframeCache &) = delete;
    ~KeyframeCache();
    int type() const { return type_; }
    void reset();
    bool observe(GstBuffer *buffer, int codec);
    GstBuffer *input(GstBuffer *au) const;
};
} // namespace amdva::nvr
