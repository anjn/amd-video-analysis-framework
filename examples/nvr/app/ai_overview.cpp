#include "ai_overview.hpp"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <set>
namespace amdva::nvr {
namespace {
const AiMetric *summary(const std::vector<AiMetric> &metrics, const char *stage) {
    for (const auto &m : metrics)
        if (m.camera.empty() && m.stage == stage)
            return &m;
    return nullptr;
}
// Images per second: calls per second times the mean images per call.
double images(const AiMetric *m) { return m ? m->rate * m->mean_batch : 0; }
} // namespace
AiOverviewSample summarize_overview(const std::vector<Status> &statuses,
                                    const std::vector<TelemetrySnapshot> &telemetry,
                                    const std::vector<AiMetric> &metrics,
                                    const std::map<std::string, PersonTrackFrame> &tracks,
                                    const Options &options, bool fatal, Clock::time_point now) {
    AiOverviewSample o;
    auto &d = o.decode;
    bool stalled = false;
    std::map<QString, unsigned> formats;
    for (const auto &s : statuses) {
        ++d.cameras;
        d.live += s.connection == "live";
        d.gaps += s.gaps;
        double fps = s.display_fps;
        for (const auto &t : telemetry)
            if (t.source == s.source.id && t.enabled) {
                const auto &decoded = t.stages[0][unsigned(TimingStage::DecodeOutput)];
                if (decoded.supported)
                    fps = decoded.fps;
                d.stops += decoded.stops;
                stalled |= decoded.stalled;
                if (t.latency[0][0].samples)
                    d.p99_ms = std::max(d.p99_ms, t.latency[0][0].p99_ms);
            }
        d.fps += fps;
        d.mpixels += fps * s.input_width * s.input_height / 1e6;
        if (s.input_width)
            ++formats[QString("%1 %2×%3")
                          .arg(QString::fromStdString(s.input_codec).toUpper())
                          .arg(s.input_width)
                          .arg(s.input_height)];
    }
    for (const auto &[format, count] : formats)
        d.formats << (count > 1 ? QString("%1 ×%2").arg(format).arg(count) : format);
    d.state = fatal                           ? AiOverviewSample::Error
              : !d.cameras                    ? AiOverviewSample::Idle
              : d.live < d.cameras || stalled ? AiOverviewSample::Warning
                                              : AiOverviewSample::Good;

    const auto &search = options.search;
    const bool mobileclip = search.embedding_model == "mobileclip2-s2";
    auto &g = o.gpu;
    if (const auto *yolo = summary(metrics, "YOLO inference")) {
        g.yolo_images = images(yolo);
        g.yolo_ms = yolo->mean_ms;
        g.yolo_batch = yolo->mean_batch;
    }
    g.reid_crops = images(summary(metrics, "ReID inference + transfer"));
    g.embedding_images =
        images(summary(metrics, mobileclip ? "Image embedding GPU" : "Image embedding"));
    g.state = fatal               ? AiOverviewSample::Error
              : g.yolo_images > 0 ? AiOverviewSample::Good
                                  : AiOverviewSample::Idle;

    auto &n = o.npu;
    n.configured = search.enabled && mobileclip &&
                   (search.mobileclip_backend == "npu" || search.mobileclip_backend == "gpu+npu");
    if (const auto *npu = summary(metrics, "Image embedding NPU")) {
        n.images = images(npu);
        n.mean_ms = npu->mean_ms;
        n.p95_ms = npu->p95_ms;
    }
    n.state = !n.configured  ? AiOverviewSample::Idle
              : fatal        ? AiOverviewSample::Error
              : n.images > 0 ? AiOverviewSample::Good
                             : AiOverviewSample::Idle;

    auto &t = o.tracking;
    t.enabled = search.enabled;
    std::set<uint64_t> people;
    for (const auto &s : statuses) {
        unsigned count = 0;
        if (auto it = tracks.find(s.source.id); it != tracks.end() && it->second.generation) {
            count = it->second.objects.size();
            for (const auto &object : it->second.objects)
                people.insert(object.person);
            t.age_ms = std::max(
                t.age_ms,
                std::chrono::duration<double, std::milli>(now - it->second.captured).count());
        }
        t.people += count;
        t.cameras.emplace_back(
            QString::fromStdString(s.source.name.empty() ? s.source.id : s.source.name), count);
    }
    t.identities = people.size();
    if (const auto *association = summary(metrics, "Tracking association"))
        t.rate = association->rate;
    t.state = !t.enabled   ? AiOverviewSample::Idle
              : fatal      ? AiOverviewSample::Error
              : t.rate > 0 ? AiOverviewSample::Good
                           : AiOverviewSample::Idle;
    return o;
}
AiOverview::AiOverview(QWidget *parent) : QWidget(parent) {
    setObjectName("ai-overview");
    setMinimumSize(480, 320);
}
void AiOverview::push(const AiOverviewSample &sample) {
    sample_ = sample;
    const double values[] = {sample.decode.fps, sample.gpu.yolo_images, sample.npu.images,
                             double(sample.tracking.people)};
    for (size_t i = 0; i < history_.size(); ++i) {
        history_[i].push_back(values[i]);
        if (history_[i].size() > history_limit)
            history_[i].pop_front();
    }
    update();
}
void AiOverview::paintEvent(QPaintEvent *) {
    struct Card {
        QString title, subtitle, value, unit;
        QStringList details;
        AiOverviewSample::State state;
    };
    auto number = [](double v) { return QString::number(v, 'f', v >= 100 || v == 0 ? 0 : 1); };
    const auto &s = sample_;
    Card cards[4];
    {
        const auto &d = s.decode;
        QStringList details{QString("%1 / %2 cameras live · %3 Mpixel/s")
                                .arg(d.live)
                                .arg(d.cameras)
                                .arg(number(d.mpixels)),
                            QString("Decoder stalls %1 · stream gaps %2 · input→decode P99 %3")
                                .arg(d.stops)
                                .arg(d.gaps)
                                .arg(d.p99_ms < 0 ? QString("N/A")
                                                  : QString::number(d.p99_ms, 'f', 1) + " ms")};
        if (!d.formats.empty())
            details << d.formats.join(" · ");
        cards[0] = {"Video decode", "VCN hardware decoder (VA-API)", number(d.fps), "frames/s",
                    details, d.state};
    }
    {
        const auto &g = s.gpu;
        cards[1] = {"GPU AI inference",
                    "YOLO detection · ReID · image embedding",
                    number(g.yolo_images),
                    "images/s",
                    {QString("YOLO %1 ms per call · batch %2")
                         .arg(g.yolo_ms, 0, 'f', 1)
                         .arg(g.yolo_batch, 0, 'f', 1),
                     QString("ReID %1 crops/s · embedding %2 images/s")
                         .arg(number(g.reid_crops))
                         .arg(number(g.embedding_images))},
                    g.state};
    }
    {
        const auto &n = s.npu;
        cards[2] = {"NPU AI inference",
                    "MobileCLIP2-S2 BF16 image embedding",
                    n.configured ? number(n.images) : "—",
                    n.configured ? "images/s" : "",
                    n.configured ? QStringList{QString("%1 ms mean · %2 ms P95 per call")
                                                   .arg(n.mean_ms, 0, 'f', 1)
                                                   .arg(n.p95_ms, 0, 'f', 1),
                                               "Runs alongside GPU detection"}
                                 : QStringList{"NPU not in use",
                                               "Select MobileCLIP2-S2 (NPU) in Detailed view"},
                    n.state};
    }
    {
        const auto &t = s.tracking;
        QStringList per_camera;
        for (const auto &[name, count] : t.cameras)
            per_camera << QString("%1 %2").arg(name).arg(count);
        cards[3] = {"Person tracking",
                    "ReID identities across cameras",
                    t.enabled ? QString::number(t.people) : "—",
                    t.enabled ? "people now" : "",
                    t.enabled ? QStringList{QString("%1 identities · %2 updates/s · age %3")
                                                .arg(t.identities)
                                                .arg(number(t.rate))
                                                .arg(t.age_ms < 0 ? QString("N/A")
                                                                  : QString::number(t.age_ms, 'f',
                                                                                    0) +
                                                                        " ms"),
                                            per_camera.join(" · ")}
                              : QStringList{"Person search is disabled"},
                    t.state};
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(16, 19, 26));
    const int gap = std::max(8, width() / 100);
    const double w = (width() - 3.0 * gap) / 2, h = (height() - 3.0 * gap) / 2;
    for (int i = 0; i < 4; ++i) {
        const QRectF box(gap + (i % 2) * (w + gap), gap + (i / 2) * (h + gap), w, h);
        const auto &c = cards[i];
        const QColor accent = c.state == AiOverviewSample::Good      ? QColor(63, 185, 80)
                              : c.state == AiOverviewSample::Warning ? QColor(210, 153, 34)
                              : c.state == AiOverviewSample::Error   ? QColor(248, 81, 73)
                                                                     : QColor(139, 148, 158);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(30, 36, 48));
        p.drawRoundedRect(box, 10, 10);
        const double pad = h * .06, unit = std::max(8., h / 22);
        auto font = [&](double size, bool bold) {
            QFont f = p.font();
            f.setPixelSize(std::max(9, int(size)));
            f.setBold(bold);
            p.setFont(f);
            return QFontMetricsF(f);
        };
        // Header: status dot, title and the hardware path.
        double y = box.top() + pad;
        auto fm = font(unit * 1.3, true);
        p.setBrush(accent);
        p.drawEllipse(QPointF(box.left() + pad + unit * .5, y + fm.ascent() * .6), unit * .45,
                      unit * .45);
        p.setPen(Qt::white);
        p.drawText(QPointF(box.left() + pad + unit * 1.4, y + fm.ascent()), c.title);
        y += fm.height();
        fm = font(unit * .95, false);
        p.setPen(QColor(150, 160, 175));
        p.drawText(QPointF(box.left() + pad, y + fm.ascent()),
                   fm.elidedText(c.subtitle, Qt::ElideRight, box.width() - 2 * pad));
        y += fm.height() + unit * .3;
        // Headline value and unit.
        fm = font(unit * 4, true);
        p.setPen(c.state == AiOverviewSample::Idle ? QColor(190, 196, 205) : Qt::white);
        p.drawText(QPointF(box.left() + pad, y + fm.ascent()), c.value);
        const double value_width = fm.horizontalAdvance(c.value);
        const double baseline = y + fm.ascent();
        y += fm.height();
        fm = font(unit * 1.4, false);
        p.setPen(accent);
        p.drawText(QPointF(box.left() + pad + value_width + unit * .6, baseline), c.unit);
        // Details.
        fm = font(unit * .95, false);
        p.setPen(QColor(205, 210, 218));
        for (const auto &line : c.details) {
            p.drawText(QPointF(box.left() + pad, y + fm.ascent()),
                       fm.elidedText(line, Qt::ElideRight, box.width() - 2 * pad));
            y += fm.height();
        }
        // Trend over the last minute, scaled to its own peak.
        const auto &values = history_[i];
        const QRectF chart(box.left() + pad, y + unit * .5, box.width() - 2 * pad,
                           box.bottom() - pad - y - unit * .5);
        if (chart.height() < unit * 2 || values.size() < 2)
            continue;
        const double peak = *std::max_element(values.begin(), values.end());
        QPainterPath line;
        for (size_t k = 0; k < values.size(); ++k) {
            const QPointF point(chart.left() + chart.width() * (history_limit - values.size() + k) /
                                                   (history_limit - 1),
                                chart.bottom() -
                                    chart.height() * .8 * values[k] / std::max(peak, 1e-6));
            k ? line.lineTo(point) : line.moveTo(point);
        }
        QPainterPath area = line;
        area.lineTo(chart.right(), chart.bottom());
        area.lineTo(line.elementAt(0).x, chart.bottom());
        area.closeSubpath();
        QColor fill = accent;
        fill.setAlpha(50);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(area);
        p.setPen(QPen(accent, std::max(1.5, unit * .15)));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
        fm = font(unit * .8, false);
        p.setPen(QColor(150, 160, 175));
        p.drawText(chart, Qt::AlignTop | Qt::AlignRight, "peak " + number(peak) + " · 60 s");
    }
}
} // namespace amdva::nvr
