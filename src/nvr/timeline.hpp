#pragma once
#include "amdva/streaming/catalog.hpp"
#include <QWidget>
#include <functional>
namespace amdva::nvr {
class RecordingTimeline final : public QWidget {
  public:
    explicit RecordingTimeline(QWidget *parent = nullptr);
    void coverage(amdva::streaming::RecordingCoverage);
    void now(int64_t);
    void position(int64_t);
    int64_t oldest() const { return from_; }
    int64_t latest() const { return to_; }
    int64_t time_at(double x) const;
    std::function<void(int64_t)> seek_requested;
    std::function<void(std::optional<int64_t>, QPoint)> preview_requested;

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

  private:
    amdva::streaming::RecordingCoverage coverage_;
    int64_t from_{}, to_{}, position_{}, hover_{};
    bool hovering_{}, dragging_{};
    double pointer_x_{};
    double x_at(int64_t) const;
};
} // namespace amdva::nvr
