#include "timeline.hpp"
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>
namespace amdva::nvr {
namespace {
constexpr int margin = 12, bar_left = 12, bar_width = 22;
} // namespace
RecordingTimeline::RecordingTimeline(QWidget *parent) : QWidget(parent) {
    setObjectName("recording-timeline");
    setMinimumHeight(180);
    setMouseTracking(true);
    setAccessibleName("Recording timeline");
}
void RecordingTimeline::coverage(amdva::streaming::RecordingCoverage c) {
    coverage_ = std::move(c);
    from_ = coverage_.oldest_ms.value_or(to_);
    if (hovering_ || dragging_)
        hover_ = time_at(pointer_y_);
    if (preview_requested && (hovering_ || dragging_))
        preview_requested(coverage_.oldest_ms ? std::optional<int64_t>(hover_) : std::nullopt,
                          mapToGlobal(QPoint(bar_left, int(pointer_y_))));
    update();
}
void RecordingTimeline::now(int64_t ms) {
    to_ = ms;
    if (!coverage_.oldest_ms)
        from_ = ms;
    if (hovering_ || dragging_) {
        hover_ = time_at(pointer_y_);
        if (preview_requested)
            preview_requested(hover_, mapToGlobal(QPoint(bar_left, int(pointer_y_))));
    }
    update();
}
void RecordingTimeline::position(int64_t ms) {
    position_ = ms;
    update();
}
int64_t RecordingTimeline::time_at(double y) const {
    const double ratio = std::clamp((y - margin) / std::max(1, height() - 2 * margin), 0., 1.);
    return to_ - int64_t(std::llround(ratio * double(std::max<int64_t>(0, to_ - from_))));
}
double RecordingTimeline::y_at(int64_t ms) const {
    return margin +
           std::clamp(double(to_ - ms) / double(std::max<int64_t>(1, to_ - from_)), 0., 1.) *
               std::max(1, height() - 2 * margin);
}
void RecordingTimeline::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(28, 32, 39));
    p.fillRect(QRect(bar_left, margin, bar_width, std::max(1, height() - 2 * margin)),
               QColor(66, 72, 82));
    if (!coverage_.oldest_ms || to_ <= from_) {
        p.setPen(Qt::lightGray);
        p.drawText(rect().adjusted(40, 0, -4, 0), Qt::AlignCenter | Qt::TextWordWrap,
                   "No playable recordings");
        return;
    }
    // Merge coverage in pixel space, traversing newest first.
    int begin = -1, end = -1;
    auto fill = [&] {
        if (begin >= 0)
            p.fillRect(QRect(bar_left, begin, bar_width, std::max(1, end - begin)),
                       QColor(45, 160, 100));
    };
    for (auto it = coverage_.intervals.rbegin(); it != coverage_.intervals.rend(); ++it) {
        const auto &r = *it;
        if (r.to_ms <= from_ || r.from_ms >= to_)
            continue;
        int a = int(std::floor(y_at(r.to_ms))), b = int(std::ceil(y_at(r.from_ms)));
        if (begin >= 0 && a <= end)
            end = std::max(end, b);
        else {
            fill();
            begin = a;
            end = b;
        }
    }
    fill();
    p.setPen(QColor(190, 198, 210));
    const int ticks = std::clamp((height() - 24) / 80, 1, 8);
    for (int i = 0; i <= ticks; ++i) {
        const int64_t t = to_ - int64_t(double(to_ - from_) * i / ticks);
        const int y = int(y_at(t));
        p.drawLine(bar_left + bar_width, y, bar_left + bar_width + 5, y);
        const auto text = QDateTime::fromMSecsSinceEpoch(t).toString("yyyy-MM-dd\nHH:mm:ss");
        p.drawText(QRect(44, std::clamp(y - 18, 0, std::max(0, height() - 38)), width() - 48, 38),
                   Qt::AlignLeft | Qt::AlignVCenter, text);
    }
    if (position_ >= from_ && position_ <= to_) {
        p.setPen(QPen(QColor(245, 245, 250), 2));
        p.drawLine(QPointF(bar_left - 4, y_at(position_)),
                   QPointF(bar_left + bar_width + 4, y_at(position_)));
    }
    if (hovering_ || dragging_) {
        p.setPen(QColor(255, 202, 91));
        const int y = int(y_at(hover_));
        p.drawLine(bar_left - 4, y, bar_left + bar_width + 4, y);
        QRect label(40, std::clamp(y - 20, 0, std::max(0, height() - 42)), width() - 44, 42);
        p.fillRect(label, QColor(48, 54, 65));
        p.drawText(label, Qt::AlignCenter,
                   QDateTime::fromMSecsSinceEpoch(hover_).toString("yyyy-MM-dd\nHH:mm:ss"));
    }
}
void RecordingTimeline::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !coverage_.oldest_ms || to_ <= from_)
        return;
    dragging_ = true;
    hovering_ = true;
    pointer_y_ = e->position().y();
    hover_ = time_at(pointer_y_);
    if (preview_requested && (hovering_ || dragging_))
        preview_requested(coverage_.oldest_ms ? std::optional<int64_t>(hover_) : std::nullopt,
                          mapToGlobal(QPoint(bar_left, int(pointer_y_))));
    update();
}
void RecordingTimeline::mouseMoveEvent(QMouseEvent *e) {
    hovering_ = true;
    pointer_y_ = e->position().y();
    hover_ = time_at(pointer_y_);
    if (preview_requested && (hovering_ || dragging_))
        preview_requested(coverage_.oldest_ms ? std::optional<int64_t>(hover_) : std::nullopt,
                          mapToGlobal(QPoint(bar_left, int(pointer_y_))));
    update();
}
void RecordingTimeline::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !dragging_)
        return;
    dragging_ = false;
    // A committed seek must keep playing while the pointer rests here.
    // Only another mouse move starts a new hover preview.
    hovering_ = false;
    pointer_y_ = e->position().y();
    hover_ = time_at(pointer_y_);
    if (seek_requested)
        seek_requested(hover_);
    update();
}
void RecordingTimeline::leaveEvent(QEvent *) {
    hovering_ = false;
    if (preview_requested)
        preview_requested({}, {});
    update();
}
} // namespace amdva::nvr
