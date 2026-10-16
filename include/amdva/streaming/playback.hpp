#pragma once
#include "amdva/streaming/controller.hpp"
namespace amdva::streaming {
// Caller holds the slot mutex. Pixels remain on VA surfaces.
void prepare_preview(GstVaDisplay *, Slot &, Plane &, GstSample *, uint64_t generation,
                     uint64_t sequence);
struct PlaybackStatus {
    std::string recording, state{"stopped"}, error;
    int64_t position_ns{}, duration_ns{-1};
    uint64_t frames{};
};
class PlaybackSession {
  public:
    explicit PlaybackSession(GstVaDisplay *);
    ~PlaybackSession();
    PlaybackSession(const PlaybackSession &) = delete;
    PlaybackSession &operator=(const PlaybackSession &) = delete;
    void open(std::shared_ptr<RecordingFile>, bool play = true);
    void open(std::shared_ptr<RecordingFile>, bool play, int64_t position_ns);
    void play();
    void pause();
    void seek(int64_t position_ns);
    void stop();
    PlaybackStatus status() const;
    Slot &slot();

  private:
    friend class TimelinePlaybackSession;
    PlaybackSession(GstVaDisplay *, Slot *);
    void output(bool enabled, uint64_t generation);
    bool ready() const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct TimelinePlaybackStatus {
    std::string channel, state{"stopped"}, error, notice;
    int64_t requested_ms{}, position_ms{};
    uint64_t frames{}, transitions{}, skipped{};
};
// Qt-free wall-time playback. Catalog must outlive this session; VA display is retained.
// One active decoder plus one bounded, paused lookahead decoder.
class TimelinePlaybackSession {
  public:
    TimelinePlaybackSession(GstVaDisplay *, Catalog &);
    ~TimelinePlaybackSession();
    TimelinePlaybackSession(const TimelinePlaybackSession &) = delete;
    TimelinePlaybackSession &operator=(const TimelinePlaybackSession &) = delete;
    void open(const std::string &channel, int64_t at_ms, bool play = true, bool follow = true);
    void seek(int64_t at_ms);
    void play();
    void pause();
    void stop();
    void follow(bool expected);
    TimelinePlaybackStatus status() const;
    Slot &slot();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva::streaming
