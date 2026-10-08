#include "amdva/streaming/playback.hpp"
#include "window.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDateTimeEdit>
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
#include <QSlider>
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
QString clock_text(int64_t ns) {
    if (ns < 0)
        return "--:--";
    return QString("%1:%2:%3")
        .arg(ns / 1000000000 / 3600, 2, 10, QChar('0'))
        .arg(ns / 1000000000 / 60 % 60, 2, 10, QChar('0'))
        .arg(ns / 1000000000 % 60, 2, 10, QChar('0'));
}
class Timeline final : public QWidget {
  public:
    std::vector<Recording> recordings;
    int64_t from{}, to{};
    std::function<void(int64_t)> clicked;
    Timeline() {
        setMinimumHeight(44);
        setToolTip("Green: finalized recording · grey: gap or unavailable · click to seek");
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(35, 38, 44));
        if (to <= from)
            return;
        for (const auto &r : recordings)
            if (r.available && r.duration_ns > 0) {
                const double x = double(r.started_ms - from) * width() / (to - from),
                             end = double(r.started_ms + r.duration_ns / 1000000 - from) * width() /
                                   (to - from);
                p.fillRect(QRectF(x, 5, std::max(1., end - x), height() - 10),
                           r.state == "finalized" ? QColor(45, 160, 100) : QColor(170, 130, 60));
            }
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (clicked && to > from)
            clicked(from + int64_t(e->position().x() / std::max(1, width()) * (to - from)));
    }
};
} // namespace
struct Window::Vms {
    Window &w;
    QTabWidget *tabs{};
    QListWidget *cameras{};
    QComboBox *layout{}, *assignment{}, *filter{};
    QLabel *tile_label{}, *storage{}, *play_status{}, *play_position{}, *timeline_info{};
    QDateTimeEdit *from{}, *to{};
    QTableWidget *recordings{};
    QSlider *seek{};
    Timeline *timeline{};
    QWidget *play_container{};
    Tiles *play_tiles{};
    std::unique_ptr<PlaybackSession> player;
    std::array<std::string, 16> cells{};
    unsigned count{4}, selected{};
    int focus{-1};
    bool ui_ready{}, dirty{}, query_pending{true}, busy{}, opening_cancelled{};
    uint64_t revision{};
    std::vector<std::pair<int64_t, std::string>> pages;
    int64_t before{INT64_MAX};
    std::string before_name, current;
    std::vector<Recording> rows, playlist;
    std::vector<std::pair<std::string, std::string>> source_names;
    int64_t pending_seek{-1};
    QString playback_notice;
    struct Result {
        bool timeline_truncated{};
        uint64_t revision;
        std::vector<Recording> rows, playlist;
        std::vector<std::pair<std::string, std::string>> cameras;
        StorageStatus storage;
    };
    std::future<Result> query;
    std::future<std::shared_ptr<RecordingFile>> opening;
    std::string opening_name;
    int64_t opening_seek{};
    unsigned ticks{};
    explicit Vms(Window &window) : w(window) {}
    void request(bool reset = true) {
        if (reset) {
            before = INT64_MAX;
            before_name.clear();
            pages.clear();
        }
        revision++;
        query_pending = true;
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
        filter->addItem("All cameras", "");
        filter->setObjectName("recording-camera");
        from = new QDateTimeEdit(QDateTime(QDate::currentDate(), QTime(0, 0)));
        to = new QDateTimeEdit(QDateTime(QDate::currentDate().addDays(1), QTime(0, 0)));
        for (auto *date : {from, to}) {
            date->setCalendarPopup(true);
            date->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
        }
        auto *search = new QPushButton("Search"), *previous_page = new QPushButton("Previous page"),
             *next_page = new QPushButton("Next page");
        filters->addWidget(filter);
        filters->addWidget(from);
        filters->addWidget(to);
        filters->addWidget(search);
        filters->addWidget(previous_page);
        filters->addWidget(next_page);
        rl->addLayout(filters);
        recordings = new QTableWidget(0, 6);
        recordings->setObjectName("recording-list");
        recordings->setHorizontalHeaderLabels(
            {"Start", "Camera", "Duration", "Size", "State", "Recording"});
        recordings->setSelectionBehavior(QAbstractItemView::SelectRows);
        recordings->setEditTriggers(QAbstractItemView::NoEditTriggers);
        recordings->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        recordings->setMaximumHeight(230);
        rl->addWidget(recordings);
        timeline_info = new QLabel("Select one camera to show recording coverage.");
        rl->addWidget(timeline_info);
        timeline = new Timeline;
        rl->addWidget(timeline);
        player = std::make_unique<PlaybackSession>(w.controller_.display());
        play_tiles = new Tiles(w.controller_, &player->slot());
        play_container = QWidget::createWindowContainer(play_tiles);
        play_container->setMinimumHeight(180);
        rl->addWidget(play_container, 1);
        auto *controls = new QHBoxLayout;
        auto *play = new QPushButton("Play"), *pause = new QPushButton("Pause"),
             *stop = new QPushButton("Stop"), *prev = new QPushButton("Previous recording"),
             *next = new QPushButton("Next recording");
        play->setObjectName("playback-play");
        pause->setObjectName("playback-pause");
        stop->setObjectName("playback-stop");
        next->setObjectName("playback-next");
        prev->setObjectName("playback-previous");
        seek = new QSlider(Qt::Horizontal);
        seek->setRange(0, 10000);
        seek->setObjectName("playback-seek");
        play_position = new QLabel;
        play_status = new QLabel;
        play_status->setObjectName("playback-status");
        for (auto *button : {prev, play, pause, stop, next})
            controls->addWidget(button);
        controls->addWidget(seek, 1);
        controls->addWidget(play_position);
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
        QObject::connect(filter, qOverload<int>(&QComboBox::activated), &w, [this] { request(); });
        QObject::connect(search, &QPushButton::clicked, &w, [this] { request(); });
        QObject::connect(next_page, &QPushButton::clicked, &w, [this] {
            if (rows.empty())
                return;
            pages.emplace_back(before, before_name);
            before = rows.back().started_ms;
            before_name = rows.back().name;
            request(false);
        });
        QObject::connect(previous_page, &QPushButton::clicked, &w, [this] {
            if (pages.empty())
                return;
            before = pages.back().first;
            before_name = pages.back().second;
            pages.pop_back();
            request(false);
        });
        QObject::connect(recordings, &QTableWidget::cellDoubleClicked, &w, [this](int row, int) {
            if (row >= 0 && row < int(rows.size()))
                open(rows[row]);
        });
        QObject::connect(play, &QPushButton::clicked, &w, [this] {
            if (current.empty()) {
                int row = recordings->currentRow();
                if (row >= 0 && row < int(rows.size()))
                    open(rows[row]);
            } else {
                if (player->status().state == "ended")
                    player->seek(0);
                player->play();
                playback_notice.clear();
            }
        });
        QObject::connect(pause, &QPushButton::clicked, &w, [this] { player->pause(); });
        QObject::connect(stop, &QPushButton::clicked, &w, [this] {
            opening_cancelled = true;
            player->stop();
            current.clear();
            pending_seek = -1;
            playback_notice.clear();
        });
        QObject::connect(next, &QPushButton::clicked, &w, [this] { advance(1, false); });
        QObject::connect(prev, &QPushButton::clicked, &w, [this] { advance(-1, false); });
        QObject::connect(seek, &QSlider::sliderReleased, &w, [this] {
            auto state = player->status();
            if (state.duration_ns > 0)
                player->seek(int64_t(double(seek->value()) / 10000 * state.duration_ns));
        });
        timeline->clicked = [this](int64_t at) {
            for (const auto &r : playlist)
                if (r.available && r.duration_ns > 0 && r.started_ms <= at &&
                    r.started_ms + r.duration_ns / 1000000 > at) {
                    open(r, (at - r.started_ms) * 1000000);
                    return;
                }
            playback_notice =
                "No recording at the selected time. Select the next available recording.";
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
    void open(const Recording &r, int64_t at = 0) {
        if (!r.available || (r.state != "finalized" && r.state != "legacy")) {
            playback_notice = "Recording is unavailable, interrupted or still being written.";
            return;
        }
        if (opening.valid()) {
            playback_notice = "Opening a recording; please wait.";
            return;
        }
        opening_name = r.name;
        opening_seek = at;
        opening_cancelled = false;
        opening = std::async(std::launch::async,
                             [this, name = r.name] { return w.controller_.open_recording(name); });
        // Selecting a row also chooses its camera for the timeline and continuation.
        int idx = filter->findData(qs(r.channel));
        if (idx >= 0 && idx != filter->currentIndex()) {
            filter->setCurrentIndex(idx);
            request();
        }
        playback_notice = "Opening recording…";
    }
    void advance(int direction, bool automatic) {
        auto it = std::find_if(playlist.begin(), playlist.end(),
                               [this](auto &r) { return r.name == current; });
        if (it == playlist.end()) {
            if (automatic)
                playback_notice = "End of loaded recording range.";
            return;
        }
        auto origin = *it;
        while (true) {
            if (direction > 0) {
                if (++it == playlist.end())
                    break;
            } else {
                if (it == playlist.begin())
                    break;
                --it;
            }
            if (!it->available || (it->state != "finalized" && it->state != "legacy"))
                continue;
            if (automatic && (origin.session.empty() || it->session != origin.session ||
                              it->fragment != origin.fragment + 1)) {
                playback_notice =
                    "Recording gap or session change. Use Next recording to continue.";
                return;
            }
            open(*it);
            return;
        }
        playback_notice = "End of loaded recording range.";
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
            }
            if (ticks % 50 == 0 && !query.valid())
                request(false);
            if (query.valid() &&
                query.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto result = query.get();
                busy = false;
                if (result.revision == revision) {
                    std::string selected_recording;
                    const int selected_row = recordings->currentRow();
                    if (selected_row >= 0 && selected_row < int(rows.size()))
                        selected_recording = rows[selected_row].name;
                    rows = std::move(result.rows);
                    playlist = std::move(result.playlist);
                    timeline->recordings = playlist;
                    timeline->from = from->dateTime().toMSecsSinceEpoch();
                    timeline->to = to->dateTime().toMSecsSinceEpoch();
                    timeline->update();
                    timeline_info->setText(result.timeline_truncated
                                               ? "Timeline limited to the latest 10,000 segments. "
                                                 "Narrow the date range."
                                           : filter->currentData().toString().isEmpty()
                                               ? "Select one camera to show recording coverage."
                                               : QString("%1 recording intervals · grey areas have "
                                                         "no known playable coverage")
                                                     .arg(playlist.size()));
                    recordings->setRowCount(rows.size());
                    for (unsigned i = 0; i < rows.size(); i++) {
                        auto &r = rows[i];
                        QStringList text{QDateTime::fromMSecsSinceEpoch(r.started_ms)
                                             .toString("yyyy-MM-dd HH:mm:ss"),
                                         qs(r.camera_name.empty() ? r.channel : r.camera_name),
                                         clock_text(r.duration_ns),
                                         size_text(r.size),
                                         r.available ? qs(r.state) : "missing",
                                         qs(r.name)};
                        for (int j = 0; j < text.size(); j++)
                            recordings->setItem(i, j, new QTableWidgetItem(text[j]));
                    }
                    if (!selected_recording.empty()) {
                        bool found = false;
                        for (unsigned i = 0; i < rows.size(); i++)
                            if (rows[i].name == selected_recording) {
                                recordings->setCurrentCell(i, 0);
                                found = true;
                                break;
                            }
                        if (!found)
                            recordings->clearSelection();
                    }
                    auto id = filter->currentData();
                    filter->clear();
                    filter->addItem("All cameras", "");
                    for (auto &c : result.cameras) {
                        auto active =
                            std::find_if(source_names.begin(), source_names.end(),
                                         [&](auto &entry) { return entry.first == c.first; });
                        filter->addItem(
                            qs(active == source_names.end() ? c.second : active->second),
                            qs(c.first));
                    }
                    filter->setCurrentIndex(std::max(0, filter->findData(id)));
                    storage->setText(QString("Recorded %1 · free %2 · quota %3 · reserved free %4")
                                         .arg(size_text(result.storage.recorded_bytes),
                                              size_text(result.storage.available_bytes),
                                              size_text(w.options_.quota),
                                              size_text(w.options_.reserve)));
                }
            }
            if (query_pending && !query.valid()) {
                query_pending = false;
                busy = true;
                RecordingQuery q;
                q.channel = filter->currentData().toString().toStdString();
                q.from_ms = from->dateTime().toMSecsSinceEpoch();
                q.to_ms = to->dateTime().toMSecsSinceEpoch();
                q.before_ms = before;
                q.before_name = before_name;
                auto generation = revision;
                if (q.to_ms < q.from_ms)
                    throw std::runtime_error("Search end must be after its start");
                auto playback = player->status();
                query = std::async(std::launch::async, [this, q, generation, playback] {
                    if (playback.frames && playback.duration_ns >= 0)
                        w.controller_.recording_duration(playback.recording, playback.duration_ns);
                    Result result;
                    result.revision = generation;
                    result.rows = w.controller_.recordings(q);
                    result.storage = w.controller_.storage();
                    result.cameras = w.controller_.recording_cameras();
                    if (!q.channel.empty()) {
                        auto t = q;
                        t.limit = 200;
                        t.before_ms = INT64_MAX;
                        t.before_name.clear();
                        while (result.playlist.size() < 10000) {
                            auto batch = w.controller_.recordings(t);
                            if (batch.empty())
                                break;
                            result.playlist.insert(result.playlist.end(), batch.begin(),
                                                   batch.end());
                            t.before_ms = batch.back().started_ms;
                            t.before_name = batch.back().name;
                            if (batch.size() < 200)
                                break;
                        }
                        result.timeline_truncated = result.playlist.size() >= 10000;
                        std::reverse(result.playlist.begin(), result.playlist.end());
                    }
                    return result;
                });
            }
            if (opening.valid() &&
                opening.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto file = opening.get();
                if (!opening_cancelled) {
                    current = opening_name;
                    pending_seek = opening_seek;
                    player->open(std::move(file));
                    playback_notice.clear();
                }
            }
            auto state = player->status();
            if (pending_seek >= 0 && (state.state == "playing" || state.state == "paused") &&
                state.recording == current) {
                if (pending_seek)
                    player->seek(pending_seek);
                pending_seek = -1;
            }
            if (state.state == "ended" && state.recording == current && opening_name == current &&
                !opening.valid() && playback_notice.isEmpty())
                advance(1, true);
            play_status->setText(
                playback_notice.isEmpty()
                    ? qs(state.state) + (state.error.empty() ? QString{} : " · " + qs(state.error))
                    : playback_notice);
            play_position->setText(clock_text(state.position_ns) + " / " +
                                   clock_text(state.duration_ns));
            if (!seek->isSliderDown())
                seek->setValue(state.duration_ns > 0 ? std::clamp(int(double(state.position_ns) /
                                                                      state.duration_ns * 10000),
                                                                  0, 10000)
                                                     : 0);
        } catch (const std::exception &e) {
            busy = false;
            playback_notice = qs(e.what());
            play_status->setText(playback_notice);
        }
    }
};
Window::Window(Controller &c, Options options, std::string config)
    : controller_(c), options_(std::move(options)), config_(std::move(config)),
      vms_(std::make_unique<Vms>(*this)) {
    vms_->build();
}
PlaybackStatus Window::playback_status() const { return vms_->player->status(); }
Tiles *Window::recording_view() const { return vms_->play_tiles; }
Window::~Window() {
    if (vms_->query.valid())
        vms_->query.wait();
    if (vms_->opening.valid())
        vms_->opening.wait();
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
