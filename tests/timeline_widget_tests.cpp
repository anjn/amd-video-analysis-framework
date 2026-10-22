#include "timeline.hpp"
#include <QApplication>
#include <QMouseEvent>
#include <iostream>
using namespace amdva::nvr;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        RecordingTimeline widget;
        widget.resize(240, 1000);
        constexpr int64_t start = 1800000000000LL, end = start + 90LL * 86400000;
        amdva::streaming::RecordingCoverage coverage;
        coverage.oldest_ms = start;
        coverage.intervals = {{start, start + 10000}, {end - 20000, end}};
        widget.now(end);
        widget.coverage(coverage);
        widget.position(start);
        check(widget.time_at(0) == end && widget.time_at(1000) == start, "Edge clamp");
        check(widget.time_at(500) == start + (end - start) / 2, "64-bit whole-history mapping");
        unsigned requests = 0, previews = 0;
        widget.preview_requested = [&](std::optional<int64_t> at, QPoint) {
            if (at)
                ++previews;
        };
        int64_t selected = 0;
        widget.seek_requested = [&](int64_t time) {
            requests++;
            selected = time;
        };
        auto event = [&](QEvent::Type type, double x, Qt::MouseButton button,
                         Qt::MouseButtons buttons) {
            QMouseEvent e(type, QPointF(24, x), QPointF(24, x), button, buttons, Qt::NoModifier);
            QApplication::sendEvent(&widget, &e);
        };
        event(QEvent::MouseMove, 500, Qt::NoButton, Qt::NoButton);
        check(!requests, "Hover seeks");
        event(QEvent::MouseButtonPress, 12, Qt::LeftButton, Qt::LeftButton);
        event(QEvent::MouseMove, 500, Qt::NoButton, Qt::LeftButton);
        check(!requests, "Drag seeks before release");
        event(QEvent::MouseButtonRelease, 988, Qt::LeftButton, Qt::NoButton);
        check(requests == 1 && selected == start, "Release commits timestamp");
        auto before = previews;
        widget.now(end + 1000);
        widget.coverage(coverage);
        check(previews == before, "Committed seek restarted hover on coverage refresh");
        event(QEvent::MouseMove, 500, Qt::NoButton, Qt::NoButton);
        check(previews > before, "Mouse movement did not restart hover");
        widget.show();
        app.processEvents();
        check(!widget.grab().isNull(), "Timeline paint");
        widget.coverage({});
        event(QEvent::MouseButtonPress, 500, Qt::LeftButton, Qt::LeftButton);
        event(QEvent::MouseButtonRelease, 500, Qt::LeftButton, Qt::NoButton);
        check(requests == 1, "Empty timeline seeks");
        std::cout << "Timeline dates, hover, drag, empty state and paint passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
