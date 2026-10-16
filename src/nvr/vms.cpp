#include "amdva/streaming/playback.hpp"
#include "timeline.hpp"
#include "window.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <future>
#include <set>
namespace amdva::nvr {
namespace {
QString qs(const std::string &s) { return QString::fromStdString(s); }
QString size_text(uint64_t n) {
    if (n >= (1ull << 30))
        return QString::number(double(n) / (1ull << 30), 'f', 2) + " GiB";
    if (n >= (1ull << 20))
        return QString::number(double(n) / (1ull << 20), 'f', 2) + " MiB";
    if (n >= 1024)
        return QString::number(double(n) / 1024, 'f', 1) + " KiB";
    return QString::number(qulonglong(n)) + " B";
}
} // namespace
struct Window::Vms {
    Window &w;
    QTabWidget *tabs{};
    QListWidget *cameras{};
    QComboBox *layout{}, *assignment{}, *filter{};
    QLabel *tile_label{}, *storage{}, *play_status{}, *play_position{}, *timeline_info{};
    RecordingTimeline *timeline{};
    QWidget *play_container{};
    Tiles *play_tiles{};
    std::unique_ptr<TimelinePlaybackSession> player;
    std::array<std::string, 16> cells{};
    unsigned count{4}, selected{};
    int focus{-1};
    bool ui_ready{}, dirty{}, query_pending{true};
    uint64_t revision{};
    int64_t cursor_ms{};
    bool camera_changed{true}, camera_playing{};
    std::vector<std::pair<std::string, std::string>> source_names;
    struct Result {
        uint64_t revision{};
        RecordingCoverage coverage;
        std::vector<std::pair<std::string, std::string>> cameras;
        StorageStatus storage;
    };
    std::future<Result> query;
    unsigned ticks{};
    explicit Vms(Window &window) : w(window) {}
    void request() {
        revision++;
        query_pending = true;
    }
    void select_camera() {
        const auto state = player->status();
        if (!camera_changed)
            camera_playing = state.state == "playing" || state.state == "waiting";
        player->stop();
        camera_changed = true;
        timeline->coverage({});
        request();
    }
    bool follows(const std::string &id) const {
        auto states = w.controller_.status();
        return std::any_of(states.begin(), states.end(),
                           [&](const auto &s) { return s.source.id == id; });
    }
    void load_ui() {
        QFile f(qs(w.config_ + ".ui.json"));
        if (f.open(QIODevice::ReadOnly)) {
            auto document = QJsonDocument::fromJson(f.readAll());
            auto obj = document.object();
            unsigned n = obj["layout"].toInt(4);
            if (n == 1 || n == 4 || n == 9 || n == 16)
                count = n;
            auto array = obj["cells"].toArray();
            std::set<std::string> seen;
            for (int i = 0; i < std::min(16, int(array.size())); i++) {
                auto id = array[i].toString().toStdString();
                if (std::any_of(w.options_.sources.begin(), w.options_.sources.end(),
                                [&](auto &s) { return s.id == id; }) &&
                    seen.insert(id).second)
                    cells[i] = id;
            }
        } else {
            unsigned i = 0;
            for (const auto &s : w.options_.sources)
                if (s.selected && i < 16)
                    cells[i++] = s.id;
            if (i)
                count = i <= 1 ? 1 : i <= 4 ? 4 : i <= 9 ? 9 : 16;
        }
    }
    void save_ui() {
        if (!dirty)
            return;
        QJsonArray array;
        for (auto &s : cells)
            array.push_back(qs(s));
        QSaveFile f(qs(w.config_ + ".ui.json"));
        if (!f.open(QIODevice::WriteOnly) ||
            !f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
            throw std::runtime_error("Cannot save view configuration");
        auto data =
            QJsonDocument(QJsonObject{{"version", 1}, {"layout", int(count)}, {"cells", array}})
                .toJson();
        if (f.write(data) != data.size() || !f.commit())
            throw std::runtime_error("Cannot save view configuration");
        dirty = false;
    }
    void apply() {
        if (!w.controller_.status_ready())
            return;
        const auto status = w.controller_.status();
        // Release first: replacement at the 16-view limit must not fail admission.
        for (const auto &s : status)
            if (s.source.selected && std::find(cells.begin(), cells.begin() + count, s.source.id) ==
                                         cells.begin() + count)
                w.controller_.select(s.source.id, false);
        for (unsigned i = 0; i < count; i++)
            if (!cells[i].empty())
                w.controller_.select(cells[i], true);
        w.tiles_->layout(cells, count, focus);
        w.tiles_->select_tile(selected);
        tile_label->setText(QString("Tile %1%2")
                                .arg(selected + 1)
                                .arg(focus >= 0 ? " · single view (Esc to return)" : ""));
    }
    void assign(const std::string &id) {
        if (selected >= count)
            return;
        if (!id.empty())
            for (unsigned i = 0; i < 16; i++)
                if (i != selected && cells[i] == id)
                    std::swap(cells[i], cells[selected]);
        cells[selected] = id;
        dirty = true;
        apply();
    }
    void edit_camera(bool edit) {
        Source source;
        if (edit) {
            int row = w.table_->currentRow();
            if (row < 0)
                return;
            auto id = w.table_->item(row, 1)->data(Qt::UserRole).toString().toStdString();
            for (auto &s : w.controller_.status())
                if (s.source.id == id)
                    source = s.source;
        } else
            source.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        QDialog dialog(&w);
        dialog.setWindowTitle(edit ? "Edit camera" : "Add camera");
        auto *form = new QFormLayout(&dialog);
        auto *name = new QLineEdit(qs(source.name)), *uri = new QLineEdit(qs(source.uri));
        uri->setMinimumWidth(440);
        form->addRow("Name", name);
        form->addRow("RTSP URL", uri);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        try {
            source.name = name->text().trimmed().toStdString();
            if (source.name.empty())
                throw std::runtime_error("Camera name is required");
            source.uri = validated_uri(uri->text().toStdString());
            if (edit)
                w.controller_.update_source(source);
            else
                w.controller_.add(source);
        } catch (const std::exception &e) {
            QMessageBox::warning(&w, "Camera configuration", qs(e.what()));
        }
    }
    void build() {
        load_ui();
        w.setWindowTitle("AMD Video Management System");
        w.resize(1500, 950);
        auto *root = new QWidget;
        auto *outer = new QVBoxLayout(root);
        w.setCentralWidget(root);
        tabs = new QTabWidget;
        tabs->setObjectName("vms-tabs");
        outer->addWidget(tabs, 1);
        storage = new QLabel;
        outer->addWidget(storage);
        auto *live = new QWidget;
        auto *live_layout = new QVBoxLayout(live);
        auto *bar = new QHBoxLayout;
        layout = new QComboBox;
        layout->setObjectName("live-layout");
        for (unsigned n : {1, 4, 9, 16})
            layout->addItem(QString("%1 views").arg(n), n);
        layout->setCurrentIndex(layout->findData(count));
        tile_label = new QLabel("Tile 1");
        assignment = new QComboBox;
        assignment->setObjectName("tile-camera");
        assignment->setMinimumWidth(240);
        auto *clear = new QPushButton("Clear tile"), *single = new QPushButton("Single / grid"),
             *settings = new QPushButton("Manage cameras");
        clear->setObjectName("tile-clear");
        bar->addWidget(layout);
        bar->addWidget(tile_label);
        bar->addWidget(assignment);
        bar->addWidget(clear);
        bar->addWidget(single);
        bar->addStretch();
        bar->addWidget(settings);
        live_layout->addLayout(bar);
        auto *split = new QSplitter;
        cameras = new QListWidget;
        cameras->setObjectName("live-cameras");
        cameras->setMaximumWidth(310);
        split->addWidget(cameras);
        w.tiles_ = new Tiles(w.controller_);
        split->addWidget(QWidget::createWindowContainer(w.tiles_));
        split->setStretchFactor(1, 1);
        live_layout->addWidget(split, 1);
        tabs->addTab(live, "Live");
        auto *record = new QWidget;
        auto *rl = new QVBoxLayout(record);
        auto *filters = new QHBoxLayout;
        filter = new QComboBox;
        filter->setObjectName("recording-camera");
        filters->addWidget(new QLabel("Camera"));
        filters->addWidget(filter, 1);
        rl->addLayout(filters);
        player = std::make_unique<TimelinePlaybackSession>(w.controller_.display(),
                                                           w.controller_.recording_catalog());
        play_tiles = new Tiles(w.controller_, &player->slot());
        play_container = QWidget::createWindowContainer(play_tiles);
        play_container->setMinimumHeight(180);
        rl->addWidget(play_container, 1);
        timeline_info = new QLabel("Green: recorded · grey: no playable recording");
        rl->addWidget(timeline_info);
        timeline = new RecordingTimeline;
        rl->addWidget(timeline);
        auto *controls = new QHBoxLayout;
        auto *play = new QPushButton("Play"), *pause = new QPushButton("Pause"),
             *stop = new QPushButton("Stop");
        play->setObjectName("playback-play");
        pause->setObjectName("playback-pause");
        stop->setObjectName("playback-stop");
        play_position = new QLabel;
        play_position->setObjectName("playback-position");
        play_status = new QLabel;
        play_status->setObjectName("playback-status");
        for (auto *button : {play, pause, stop})
            controls->addWidget(button);
        controls->addWidget(play_position, 1);
        rl->addLayout(controls);
        rl->addWidget(play_status);
        tabs->addTab(record, "Recordings");
        auto *manage = new QWidget;
        auto *ml = new QVBoxLayout(manage);
        auto *buttons = new QHBoxLayout;
        auto *add = new QPushButton("Add camera"), *edit = new QPushButton("Edit camera"),
             *remove = new QPushButton("Remove camera");
        add->setObjectName("camera-add");
        edit->setObjectName("camera-edit");
        remove->setObjectName("camera-remove");
        buttons->addWidget(add);
        buttons->addWidget(edit);
        buttons->addWidget(remove);
        buttons->addStretch();
        ml->addLayout(buttons);
        w.table_ = new QTableWidget(0, 8);
        w.table_->setObjectName("camera-list");
        w.table_->setHorizontalHeaderLabels({"Live", "Camera", "Connection", "Recording",
                                             "Decode / swap / AI FPS", "Status",
                                             "Input→decode P99 ms", "Input→swap P99 ms"});
        w.table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        w.table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        w.table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        ml->addWidget(w.table_);
        w.summary_ = new QLabel;
        w.summary_->setWordWrap(true);
        ml->addWidget(w.summary_);
        tabs->addTab(manage, "Cameras");
        QObject::connect(add, &QPushButton::clicked, &w, [this] { edit_camera(false); });
        QObject::connect(edit, &QPushButton::clicked, &w, [this] { edit_camera(true); });
        QObject::connect(settings, &QPushButton::clicked, &w, [this] { tabs->setCurrentIndex(2); });
        QObject::connect(remove, &QPushButton::clicked, &w, [this] {
            int row = w.table_->currentRow();
            if (row < 0)
                return;
            auto id = w.table_->item(row, 1)->data(Qt::UserRole).toString().toStdString();
            if (QMessageBox::question(
                    &w, "Remove camera",
                    "Stop receiving this camera? Existing recordings will be kept.") !=
                QMessageBox::Yes)
                return;
            for (auto &cell : cells)
                if (cell == id)
                    cell.clear();
            w.controller_.remove(id);
            dirty = true;
            apply();
            request();
        });
        QObject::connect(layout, qOverload<int>(&QComboBox::currentIndexChanged), &w, [this] {
            count = layout->currentData().toUInt();
            selected = std::min(selected, count - 1);
            focus = -1;
            dirty = true;
            apply();
        });
        QObject::connect(assignment, qOverload<int>(&QComboBox::activated), &w,
                         [this] { assign(assignment->currentData().toString().toStdString()); });
        QObject::connect(clear, &QPushButton::clicked, &w, [this] { assign(""); });
        w.tiles_->activated = [this](unsigned index, bool twice) {
            selected = std::min(index, count - 1);
            if (twice)
                focus = focus < 0 ? int(selected) : -1;
            apply();
            refresh_sources();
        };
        QObject::connect(single, &QPushButton::clicked, &w,
                         [this] { w.tiles_->activated(selected, true); });
        QObject::connect(cameras, &QListWidget::itemDoubleClicked, &w,
                         [this](QListWidgetItem *item) {
                             assign(item->data(Qt::UserRole).toString().toStdString());
                         });
        QObject::connect(tabs, &QTabWidget::currentChanged, &w, [this](int index) {
            if (index != 1)
                player->pause();
            if (index == 1)
                request();
            apply();
        });
        QObject::connect(filter, qOverload<int>(&QComboBox::activated), &w,
                         [this] { select_camera(); });
        QObject::connect(play, &QPushButton::clicked, &w, [this] {
            camera_playing = true;
            auto id = filter->currentData().toString().toStdString();
            if (id.empty() || !timeline->oldest() || timeline->oldest() >= timeline->latest())
                return;
            auto state = player->status();
            if (state.channel != id || state.state == "stopped" || state.state == "ended" ||
                state.state == "error")
                player->open(id, cursor_ms ? cursor_ms : timeline->oldest(), true, follows(id));
            else
                player->play();
        });
        QObject::connect(pause, &QPushButton::clicked, &w, [this] {
            camera_playing = false;
            player->pause();
        });
        QObject::connect(stop, &QPushButton::clicked, &w, [this] {
            camera_changed = false;
            camera_playing = false;
            player->stop();
        });
        timeline->seek_requested = [this](int64_t at) {
            auto id = filter->currentData().toString().toStdString();
            if (id.empty())
                return;
            cursor_ms = at;
            bool play = player->status().state != "paused";
            player->open(id, at, play, follows(id));
        };
        auto *timer = new QTimer(&w);
        QObject::connect(timer, &QTimer::timeout, &w, [this] { tick(); });
        timer->start(100);
        auto *paint = new QTimer(&w);
        QObject::connect(paint, &QTimer::timeout, &w, [this] {
            w.tiles_->update();
            play_tiles->update();
        });
        paint->start(33);
    }
    void refresh_sources() {
        const auto status = w.controller_.status();
        std::vector<std::pair<std::string, std::string>> names;
        for (const auto &s : status)
            names.emplace_back(s.source.id, s.source.name);
        if (names != source_names) {
            const auto chosen = cameras->currentItem()
                                    ? cameras->currentItem()->data(Qt::UserRole).toString()
                                    : QString{};
            cameras->clear();
            assignment->clear();
            assignment->addItem("Empty tile", "");
            for (auto &s : status) {
                auto *item = new QListWidgetItem(cameras);
                item->setData(Qt::UserRole, qs(s.source.id));
                if (item->data(Qt::UserRole).toString() == chosen)
                    cameras->setCurrentItem(item);
                assignment->addItem(qs(s.source.name), qs(s.source.id));
            }
            source_names = std::move(names);
        }
        for (unsigned i = 0; i < status.size(); i++)
            cameras->item(i)->setText(qs(status[i].source.name) + " · " + qs(status[i].connection));
        if (!assignment->view()->isVisible())
            assignment->setCurrentIndex(std::max(0, assignment->findData(qs(cells[selected]))));
    }
    void tick() {
        try {
            if (!ui_ready && w.controller_.status_ready()) {
                ui_ready = true;
                apply();
                refresh_sources();
            }
            if (++ticks % 10 == 0) {
                w.refresh();
                refresh_sources();
                apply();
                save_ui();
                w.persist();
                timeline->now(QDateTime::currentMSecsSinceEpoch());
                if (!query.valid())
                    query_pending = true;
                player->follow(follows(filter->currentData().toString().toStdString()));
            }
            if (query.valid() &&
                query.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto result = query.get();
                if (result.revision == revision) {
                    const auto chosen = filter->currentData();
                    filter->clear();
                    std::set<std::string> added;
                    for (const auto &entry : source_names) {
                        filter->addItem(qs(entry.second), qs(entry.first));
                        added.insert(entry.first);
                    }
                    for (const auto &entry : result.cameras)
                        if (added.insert(entry.first).second)
                            filter->addItem(qs(entry.second), qs(entry.first));
                    auto index = filter->findData(chosen);
                    filter->setCurrentIndex(index >= 0 ? index : (filter->count() ? 0 : -1));
                    if (filter->currentData() != chosen) {
                        select_camera();
                    } else {
                        timeline->now(QDateTime::currentMSecsSinceEpoch());
                        const auto oldest = result.coverage.oldest_ms;
                        timeline->coverage(std::move(result.coverage));
                        if (camera_changed && oldest) {
                            if (!cursor_ms)
                                cursor_ms = *oldest;
                            const auto id = filter->currentData().toString().toStdString();
                            player->open(id, cursor_ms, camera_playing, follows(id));
                            camera_changed = false;
                        }
                        timeline_info->setText(result.coverage.pending_durations
                                                   ? "Some older recordings have unknown duration "
                                                     "· green: recorded · grey: unavailable"
                                                   : "Green: recorded · grey: no playable "
                                                     "recording · gaps are skipped");
                    }
                    storage->setText(QString("Recorded %1 · free %2 · quota %3 · reserved free %4")
                                         .arg(size_text(result.storage.recorded_bytes),
                                              size_text(result.storage.available_bytes),
                                              size_text(w.options_.quota),
                                              size_text(w.options_.reserve)));
                }
            }
            if (query_pending && !query.valid()) {
                query_pending = false;
                auto channel = filter->currentData().toString().toStdString();
                auto generation = revision;
                query = std::async(std::launch::async, [this, channel, generation] {
                    auto &catalog = w.controller_.recording_catalog();
                    if (!channel.empty())
                        catalog.index_unknown(channel);
                    Result out;
                    out.revision = generation;
                    if (!channel.empty())
                        out.coverage = catalog.coverage(channel);
                    out.cameras = catalog.cameras();
                    out.storage = catalog.storage();
                    return out;
                });
            }
            auto state = player->status();
            if (!camera_changed &&
                state.channel == filter->currentData().toString().toStdString() &&
                state.state != "stopped")
                cursor_ms = state.position_ms;
            timeline->position(cursor_ms);
            play_position->setText(cursor_ms ? QDateTime::fromMSecsSinceEpoch(cursor_ms).toString(
                                                   "yyyy-MM-dd HH:mm:ss")
                                             : QString{});
            QString label = state.state == "waiting" ? "Waiting for the next finalized recording…"
                                                     : qs(state.state);
            if (!state.notice.empty())
                label += " · " + qs(state.notice);
            if (!state.error.empty())
                label += " · " + qs(state.error);
            play_status->setText(label);
        } catch (const std::exception &e) {
            play_status->setText(qs(e.what()));
        }
    }
};
Window::Window(Controller &c, Options options, std::string config)
    : controller_(c), options_(std::move(options)), config_(std::move(config)),
      vms_(std::make_unique<Vms>(*this)) {
    vms_->build();
}
TimelinePlaybackStatus Window::timeline_status() const { return vms_->player->status(); }
Tiles *Window::recording_view() const { return vms_->play_tiles; }
Window::~Window() {
    if (vms_->query.valid())
        vms_->query.wait();
    // GL users must release their surfaces before player/controller destruction.
    delete takeCentralWidget();
    tiles_ = nullptr;
    vms_->play_tiles = nullptr;
    vms_->player.reset();
    controller_.shutdown();
    try {
        vms_->save_ui();
        persist();
    } catch (...) {
    }
}
void Window::persist() {
    if (!controller_.status_ready() || !controller_.fatal().empty())
        return;
    const auto status = controller_.status();
    std::vector<Source> sources;
    for (const auto &s : status)
        sources.push_back(s.source);
    bool same = sources.size() == options_.sources.size();
    if (same)
        for (unsigned i = 0; i < sources.size(); ++i) {
            const auto &a = sources[i];
            const auto &b = options_.sources[i];
            same &= a.id == b.id && a.name == b.name && a.uri == b.uri && a.selected == b.selected;
        }
    if (same)
        return;
    const auto previous = options_.sources;
    options_.sources = std::move(sources);
    try {
        save_config(config_, options_);
    } catch (...) {
        options_.sources = previous;
        throw;
    }
}
void Window::refresh() {
    const auto statuses = controller_.status();
    const auto timing_snapshots = controller_.telemetry();
    updating_ = true;
    table_->setRowCount(int(statuses.size()));
    for (unsigned row = 0; row < statuses.size(); ++row) {
        const auto &s = statuses[row];
        for (unsigned col = 0; col < 8; ++col)
            if (!table_->item(row, col))
                table_->setItem(row, col, new QTableWidgetItem);
        auto *check = table_->item(row, 0);
        check->setFlags(Qt::ItemIsEnabled);
        check->setCheckState(s.source.selected ? Qt::Checked : Qt::Unchecked);
        table_->item(row, 1)->setText(qs(s.source.name));
        table_->item(row, 1)->setData(Qt::UserRole, qs(s.source.id));
        table_->item(row, 2)->setText(qs(s.connection));
        table_->item(row, 3)->setText(qs(s.recording));
        table_->item(row, 4)->setText(
            QString("%1 / %2").arg(s.display_fps, 0, 'f', 1).arg(s.ai_fps, 0, 'f', 1));
        for (const auto &metrics : timing_snapshots)
            if (metrics.source == s.source.id) {
                auto text = [](const TimingDistribution &d) {
                    return d.samples ? QString::number(d.p99_ms, 'f', 1) : QString("N/A");
                };
                table_->item(row, 4)->setText(
                    QString("decode %1 / swap %2 / AI %3")
                        .arg(metrics.stages[0][unsigned(TimingStage::DecodeOutput)].fps, 0, 'f', 1)
                        .arg(metrics.stages[0][unsigned(TimingStage::Swap)].fps, 0, 'f', 1)
                        .arg(s.ai_fps, 0, 'f', 1));
                table_->item(row, 6)->setText(text(metrics.latency[0][0]));
                table_->item(row, 7)->setText(text(metrics.latency[0][3]));
                if (!metrics.enabled)
                    table_->item(row, 4)->setText(
                        QString("Timing off / AI %1").arg(s.ai_fps, 0, 'f', 1));
                QString details = "Latency: mean / P95 / P99 / maximum (ms)\n";
                const char *names[] = {"Input → decode", "Decoder input → output", "Input → draw",
                                       "Input → swap",   "Decode → preview",       "Preview → draw",
                                       "Draw → swap"};
                for (unsigned i = 0; i < 7; i++) {
                    const auto &d = metrics.latency[0][i];
                    details += QString::fromUtf8(names[i]) + ": ";
                    if (!d.samples)
                        details += "N/A\n";
                    else
                        details += QString("%1 / %2 / %3 / %4 (%5 samples%6)\n")
                                       .arg(d.mean_ms, 0, 'f', 1)
                                       .arg(d.p95_ms, 0, 'f', 1)
                                       .arg(d.p99_ms, 0, 'f', 1)
                                       .arg(d.maximum_ms, 0, 'f', 1)
                                       .arg(qulonglong(d.samples))
                                       .arg(d.truncated ? ", partial window" : "");
                }
                for (auto stage : {TimingStage::DecodeOutput, TimingStage::Swap}) {
                    const auto &v = metrics.stages[0][unsigned(stage)];
                    details +=
                        QString("%1: %2 frames; interval P99 %3 ms; %4 stops, longest %5 ms%6\n")
                            .arg(stage == TimingStage::Swap ? "New-image swap" : "Decoder output")
                            .arg(qulonglong(v.count))
                            .arg(text(v.intervals))
                            .arg(qulonglong(v.stops))
                            .arg(v.longest_stop_ms, 0, 'f', 1)
                            .arg(!v.supported ? " (unavailable)"
                                 : !v.active  ? " (paused)"
                                              : "");
                }
                details +=
                    QString("Discards: compressed %1, latest %2, preview %3\nUnmatched: RTP %4, "
                            "decoder %5; ambiguous/missing PTS %6\nBackground: %7 decoded, input → "
                            "decode P99 %8 ms\nRecent window: up to 10 seconds / 4096 samples")
                        .arg(qulonglong(metrics.encoded_discarded))
                        .arg(qulonglong(metrics.latest_replaced))
                        .arg(qulonglong(metrics.preview_discarded))
                        .arg(qulonglong(metrics.unmatched_rtp))
                        .arg(qulonglong(metrics.unmatched_decode))
                        .arg(qulonglong(metrics.ambiguous_pts))
                        .arg(qulonglong(
                            metrics.stages[1][unsigned(TimingStage::DecodeOutput)].count))
                        .arg(text(metrics.latency[1][0]));
                for (unsigned col : {4u, 6u, 7u})
                    table_->item(row, col)->setToolTip(details);
            }
        table_->item(row, 5)->setText(
            qs(s.error) + QString(" · gaps %1 · AI age %2 ms").arg(s.gaps).arg(s.ai_age_ms) +
            (options_.background_ai_enabled ? QString(" · BG %1 · runs %2 · boxes %3 · age %4 ms")
                                                  .arg(qs(s.background_state))
                                                  .arg(s.background_ai)
                                                  .arg(s.background_detections.size())
                                                  .arg(s.background_age_ms)
                                            : QString{}));
    }
    updating_ = false;
    const auto publication = controller_.publication_status();
    summary_->setText(QString("%1 sources · batch %2 · %3 ms\nResults: %6 delivered · %7 dropped · "
                              "%8 errors\n%4\n%5\n%9")
                          .arg(statuses.size())
                          .arg(controller_.last_batch())
                          .arg(controller_.batch_ms(), 0, 'f', 1)
                          .arg(qs(controller_.fatal()))
                          .arg(qs(tiles_->error()))
                          .arg(publication.delivered)
                          .arg(publication.dropped)
                          .arg(publication.errors)
                          .arg(qs(publication.last_error)));
    static unsigned ticks = 0;
    if (++ticks % 30 == 0) {
        try {
            persist();
        } catch (const std::exception &e) {
            summary_->setText(qs(e.what()));
        }
    }
}

} // namespace amdva::nvr
