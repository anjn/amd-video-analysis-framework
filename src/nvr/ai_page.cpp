#include "ai_page.hpp"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace amdva::nvr {
AiPage::AiPage(Controller &controller, const Options &options,
               std::function<void(double, unsigned)> changed,
               std::function<bool(const std::string &, const std::string &)> model_changed,
               QWidget *parent)
    : QWidget(parent) {
    setObjectName("ai-statistics");
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    auto *fps = new QDoubleSpinBox;
    fps->setObjectName("ai-frequency");
    fps->setRange(.1, 10);
    fps->setDecimals(1);
    fps->setSingleStep(.1);
    fps->setSuffix(" Hz / camera");
    fps->setValue(options.ai_fps);
    auto *interval = new QDoubleSpinBox;
    interval->setObjectName("embedding-interval");
    interval->setRange(.1, 60);
    interval->setDecimals(1);
    interval->setSingleStep(.1);
    interval->setSuffix(" sec / person");
    interval->setValue(options.search.interval_ms / 1000.);
    auto *model = new QComboBox;
    model->setObjectName("embedding-model");
    model->setEnabled(false);
    model->addItem("EmbeddingGemma 2 (GPU)", "embeddinggemma2");
    model->addItem("MobileCLIP2-S2 (GPU FP16)", "gpu");
    model->addItem("MobileCLIP2-S2 (NPU BF16)", "npu");
    const int active_model = options.search.embedding_model == "embeddinggemma2" ? 0
                             : options.search.mobileclip_backend == "npu"        ? 2
                                                                                 : 1;
    model->setCurrentIndex(active_model);
    form->addRow("Embedding model (next startup)", model);
    auto *restart = new QLabel;
    restart->setWordWrap(true);
    form->addRow(restart);
    connect(
        model, qOverload<int>(&QComboBox::activated), this,
        [model, restart, active_model, model_changed, previous = active_model](int index) mutable {
            const std::string name = index == 0 ? "embeddinggemma2" : "mobileclip2-s2";
            const std::string backend = index == 2 ? "npu" : "gpu";
            if (!model_changed(name, backend)) {
                model->setCurrentIndex(previous);
                return;
            }
            previous = index;
            restart->setText(index == active_model
                                 ? ""
                                 : "Saved. Restart the application to change the embedding model.");
        });
    form->addRow("Detection / tracking", fps);
    form->addRow("Requested embedding interval", interval);
    fps->setEnabled(false);
    interval->setEnabled(false);
    layout->addLayout(form);
    auto *state = new QLabel;
    state->setWordWrap(true);
    layout->addWidget(state);
    auto info = ModelInfo::load(options.model_info);
    auto precision = [](const std::string &path) {
        QFile file(QString::fromStdString(path + ".manifest.json"));
        if (!file.open(QIODevice::ReadOnly))
            return QString("unknown precision");
        return QJsonDocument::fromJson(file.readAll())
            .object()
            .value("precision")
            .toString("unknown precision")
            .toUpper();
    };
    auto *models = new QLabel(
        QString("YOLO: %1 · %9 · %2×%3 · batch ≤%4\nReID: %5 · %10\nEmbedding: %6×%7 · batch %8 "
                "(padded) · "
                "token "
                "budget 70\nCompleted wall time over 60 seconds; not GPU utilization. Wait "
                "includes queue delay; per-camera update rows show frame age.")
            .arg(QFileInfo(QString::fromStdString(options.model)).fileName())
            .arg(info.width)
            .arg(info.height)
            .arg(options.batch)
            .arg(QFileInfo(QString::fromStdString(options.search.reid_model)).fileName())
            .arg(options.search.embedding_width ? options.search.embedding_width
                                                : options.search.image_size)
            .arg(options.search.embedding_height ? options.search.embedding_height
                                                 : options.search.image_size)
            .arg(options.search.embedding_batch)
            .arg(precision(options.model))
            .arg(precision(options.search.reid_model)));
    if (options.search.embedding_model == "mobileclip2-s2") {
        const auto text = models->text();
        const auto begin = text.indexOf("Embedding:");
        const auto end = text.indexOf("\nCompleted", begin);
        models->setText(
            text.left(begin) +
            QString("Embedding: MobileCLIP2-S2 · %1 · 512D · 256×256 · image batch %2 · CPU text "
                    "encoder")
                .arg(options.search.mobileclip_backend == "npu" ? "NPU BF16" : "GPU FP16")
                .arg(options.search.mobileclip_backend == "npu" ? 1
                                                                : options.search.embedding_batch) +
            text.mid(end));
    } else {
        models->setText(
            models->text().replace("Embedding:", "Embedding: EmbeddingGemma 2 · GPU · 768D ·"));
    }
    models->setWordWrap(true);
    layout->addWidget(models);
    auto *camera = new QTableWidget(0, 5);
    camera->setHorizontalHeaderLabels(
        {"Camera", "Detection updates/s", "People", "Tracking age (ms)", "Tracking updates/s"});
    camera->setEditTriggers(QAbstractItemView::NoEditTriggers);
    camera->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    camera->setMaximumHeight(230);
    layout->addWidget(camera);
    auto *detail = new QComboBox;
    detail->addItems({"Stage summary", "All stages and cameras"});
    layout->addWidget(detail);
    auto *table = new QTableWidget(0, 12);
    table->setObjectName("ai-metrics");
    table->setHorizontalHeaderLabels({"Stage", "Camera", "Mean ms", "P50 ms", "P95 ms", "Max ms",
                                      "Wait ms", "Calls/s", "Mean batch", "Pending", "Dropped",
                                      "Samples"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->verticalHeader()->setDefaultSectionSize(24);
    camera->verticalHeader()->setDefaultSectionSize(24);
    layout->addWidget(table, 1);
    auto change = [fps, interval, changed] {
        changed(fps->value(), unsigned(std::lround(interval->value() * 1000)));
    };
    connect(fps, &QDoubleSpinBox::editingFinished, this, change);
    connect(interval, &QDoubleSpinBox::editingFinished, this, change);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [&, state, table, camera, detail, fps, interval, model] {
        const bool ready = controller.status_ready() && controller.fatal().empty();
        fps->setEnabled(ready);
        model->setEnabled(ready);
        interval->setEnabled(ready);
        if (!isVisible())
            return;
        auto runtime = controller.ai_runtime();
        auto metrics = runtime->snapshot();
        QString text = QString("Requested %1 Hz · embedding requested %2 s / effective %3 s · "
                               "priority: tracking → query → collection")
                           .arg(runtime->fps())
                           .arg(runtime->interval_ms() / 1000.)
                           .arg(runtime->effective_interval() / 1000.);
        if (auto *s = controller.search_service()) {
            auto status = s->status();
            text += "\nSearch: " + QString::fromStdString(status.state) + " · " +
                    QString::fromStdString(status.error);
            if (!status.reid_shape.empty())
                text += " · ReID NCHW: " + QString::fromStdString(status.reid_shape);
        }
        if (!controller.fatal().empty())
            text += "\nAI error: " + QString::fromStdString(controller.fatal());
        state->setText(text);
        auto displayed = metrics;
        if (detail->currentIndex() == 0)
            displayed.erase(std::remove_if(displayed.begin(), displayed.end(),
                                           [](const auto &m) { return !m.camera.empty(); }),
                            displayed.end());
        const std::vector<std::string> stages{"YOLO preprocess",
                                              "YOLO inference",
                                              "YOLO postprocess",
                                              "YOLO executor total",
                                              "Person crop",
                                              "ReID preprocess",
                                              "ReID inference + transfer",
                                              "Tracking association",
                                              "Image embedding",
                                              "Text query",
                                              "Image query",
                                              "Vector search",
                                              "PNG + DB save",
                                              "Tracking queue",
                                              "Embedding queue",
                                              "Detection update"};
        std::stable_sort(displayed.begin(), displayed.end(), [&](const auto &a, const auto &b) {
            return std::find(stages.begin(), stages.end(), a.stage) <
                   std::find(stages.begin(), stages.end(), b.stage);
        });
        table->setRowCount(displayed.size());
        auto set = [](QTableWidget *t, int r, int c, const QString &v) {
            if (!t->item(r, c))
                t->setItem(r, c, new QTableWidgetItem);
            t->item(r, c)->setText(v);
        };
        for (size_t i = 0; i < displayed.size(); ++i) {
            const auto &m = displayed[i];
            QStringList row{QString::fromStdString(m.stage),
                            m.camera.empty() ? "All" : QString::fromStdString(m.camera)};
            for (double v :
                 {m.mean_ms, m.p50_ms, m.p95_ms, m.max_ms, m.wait_ms, m.rate, m.mean_batch})
                row << QString::number(v, 'f', 2);
            row << QString::number(m.pending) << QString::number(m.dropped)
                << QString::number(m.samples) + (m.truncated ? " (capped)" : "");
            for (int c = 0; c < row.size(); ++c)
                set(table, i, c, row[c]);
        }
        auto sources = controller.status();
        camera->setRowCount(sources.size());
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto &s = sources[i];
            double detection = 0, tracking = 0;
            for (const auto &m : metrics)
                if (m.camera == s.source.id) {
                    if (m.stage == "Detection update")
                        detection = m.rate;
                    if (m.stage == "Tracking association")
                        tracking = m.rate;
                }
            streaming::PersonTrackFrame f;
            if (auto *search = controller.search_service())
                f = search->tracks(s.source.id);
            set(camera, i, 0,
                QString::fromStdString(s.source.name.empty() ? s.source.id : s.source.name));
            set(camera, i, 1, QString::number(detection, 'f', 2));
            set(camera, i, 2, QString::number(f.objects.size()));
            set(camera, i, 3,
                f.generation ? QString::number(std::chrono::duration<double, std::milli>(
                                                   Clock::now() - f.captured)
                                                   .count(),
                                               'f', 0)
                             : "N/A");
            set(camera, i, 4, QString::number(tracking, 'f', 2));
        }
    });
    timer->start(1000);
}
} // namespace amdva::nvr
