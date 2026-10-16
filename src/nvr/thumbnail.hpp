#pragma once
#include "amdva/streaming/playback.hpp"
#include <QObject>
#include <QPoint>
class QWidget;
namespace amdva::nvr {
class ThumbnailWindow;
// Native GPU child overlay: movable on Wayland without remapping a tooltip.
class ThumbnailPopup final : public QObject {
  public:
    explicit ThumbnailPopup(QWidget *parent);
    ~ThumbnailPopup() override;
    void display(const amdva::streaming::KeyframePreviewStatus &, const QPoint &anchor);
    void clear();
    std::string error() const;
    bool exposed() const;
    bool isVisible() const;
    std::shared_ptr<const amdva::streaming::GpuPreviewImage> displayed_image() const;
    std::function<void(uint64_t)> presented;

  private:
    ThumbnailWindow *view_{};
    uint64_t image_revision_{};
};
} // namespace amdva::nvr
