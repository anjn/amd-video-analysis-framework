#pragma once
#include "amdva/streaming/playback.hpp"
#include <QWidget>
namespace amdva::nvr {
class SearchPage final : public QWidget {
  public:
    SearchPage(streaming::Controller &, QWidget *parent = nullptr);
    ~SearchPage() override;
    void search_image(streaming::SearchImage);
    void leave();
    streaming::TimelinePlaybackStatus playback_status() const;
    std::string renderer_status() const;
    uint64_t presented() const;
    std::future<void> release();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva::nvr
