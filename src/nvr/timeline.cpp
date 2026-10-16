#include "timeline.hpp"
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>
namespace amdva::nvr {
namespace {
constexpr int margin = 12, bar_top = 38, bar_height = 22;
QString stamp(int64_t ms) {
    return QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm:ss");
}
} // namespace
RecordingTimeline::RecordingTimeline(QWidget *parent) : QWidget(parent) {
    setObjectName("recording-timeline");
    setMinimumHeight(100);
    setMouseTracking(true);
    setAccessibleName("Recording timeline");
}
void RecordingTimeline::coverage(amdva::streaming::RecordingCoverage c) {
    coverage_ = std::move(c);
    from_ = coverage_.oldest_ms.value_or(to_);
    if (hovering_ || dragging_)
        hover_ = time_at(pointer_x_);
    update();
}
void RecordingTimeline::now(int64_t ms) {
    to_ = ms;
    if (!coverage_.oldest_ms)
        from_ = ms;
    if (hovering_ || dragging_)
        hover_ = time_at(pointer_x_);
    update();
}
void RecordingTimeline::position(int64_t ms) {
    position_ = ms;
    update();
}
int64_t RecordingTimeline::time_at(double x) const {
    const double ratio = std::clamp((x - margin) / std::max(1, width() - 2 * margin), 0., 1.);
    return from_ + int64_t(std::llround(ratio * double(std::max<int64_t>(0, to_ - from_))));
}
double RecordingTimeline::x_at(int64_t ms) const {
    return margin +
           std::clamp(double(ms - from_) / double(std::max<int64_t>(1, to_ - from_)), 0., 1.) *
               std::max(1, width() - 2 * margin);
}
void RecordingTimeline::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(28, 32, 39));
    p.fillRect(QRect(margin, bar_top, std::max(1, width() - 2 * margin), bar_height),
               QColor(66, 72, 82));
    if (!coverage_.oldest_ms || to_ <= from_) {
        p.setPen(Qt::lightGray);
        p.drawText(rect(), Qt::AlignCenter, "No playable recordings");
        return;
    }
    // Merge in pixel space as well; long histories cannot produce unbounded paint calls.
    int begin = -1, end = -1;
    auto fill = [&] {
        if (begin >= 0)
            p.fillRect(QRect(begin, bar_top, std::max(1, end - begin), bar_height),
                       QColor(45, 160, 100));
    };
    for (const auto &r : coverage_.intervals) {
        if (r.to_ms <= from_ || r.from_ms >= to_)
            continue;
        int a = int(std::floor(x_at(r.from_ms))), b = int(std::ceil(x_at(r.to_ms)));
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
    for (int i = 0; i <= 4; i++) {
        int64_t t = from_ + int64_t(double(to_ - from_) * i / 4);
        int x = int(x_at(t));
        p.drawLine(x, bar_top + bar_height, x, bar_top + bar_height + 5);
        if (width() < 850 && i != 0 && i != 4)
            continue;
        QString text = stamp(t);
        int tw = p.fontMetrics().horizontalAdvance(text);
        p.drawText(std::clamp(x - tw / 2, margin, std::max(margin, width() - margin - tw)), 84,
                   text);
    }
    if (position_ >= from_ && position_ <= to_) {
        p.setPen(QPen(QColor(245, 245, 250), 2));
        p.drawLine(QPointF(x_at(position_), bar_top - 4),
                   QPointF(x_at(position_), bar_top + bar_height + 4));
    }
    if (hovering_ || dragging_) {
        p.setPen(QColor(255, 202, 91));
        double x = x_at(hover_);
        p.drawLine(QPointF(x, bar_top - 8), QPointF(x, bar_top + bar_height + 4));
        QString text = stamp(hover_);
        int tw = p.fontMetrics().horizontalAdvance(text) + 16;
        int left = std::clamp(int(x) - tw / 2, 0, std::max(0, width() - tw));
        p.fillRect(QRect(left, 4, tw, 26), QColor(48, 54, 65));
        p.drawText(QRect(left, 4, tw, 26), Qt::AlignCenter, text);
    }
}
void RecordingTimeline::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !coverage_.oldest_ms || to_ <= from_)
        return;
    dragging_ = true;
    hovering_ = true;
    pointer_x_ = e->position().x();
    hover_ = time_at(pointer_x_);
    update();
}
void RecordingTimeline::mouseMoveEvent(QMouseEvent *e) {
    hovering_ = true;
    pointer_x_ = e->position().x();
    hover_ = time_at(pointer_x_);
    update();
}
void RecordingTimeline::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !dragging_)
        return;
    dragging_ = false;
    pointer_x_ = e->position().x();
    hover_ = time_at(pointer_x_);
    if (seek_requested)
        seek_requested(hover_);
    update();
}
void RecordingTimeline::leaveEvent(QEvent *) {
    hovering_ = false;
    update();
}
} // namespace amdva::nvr
