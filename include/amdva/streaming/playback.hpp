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
    void play();
    void pause();
    void seek(int64_t position_ns);
    void stop();
    PlaybackStatus status() const;
    Slot &slot();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva::streaming
