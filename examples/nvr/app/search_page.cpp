#include "search_page.hpp"
#include "flow_layout.hpp"
#include "search_conditions.hpp"
#include "window.hpp"
#include <QButtonGroup>
#include <QDateTime>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
namespace amdva::nvr {
using namespace amdva::nvr;
namespace {
constexpr int card_width = 212, card_image_width = 196, card_image_height = 238;
constexpr int strip_image_width = 45, strip_image_height = 80;
QString full_time(int64_t t) {
    return QDateTime::fromMSecsSinceEpoch(t).toString("yyyy-MM-dd HH:mm:ss");
}
QString short_time(int64_t t) {
    const auto when = QDateTime::fromMSecsSinceEpoch(t);
    return when.toString(when.date() == QDate::currentDate() ? "HH:mm:ss" : "MM-dd HH:mm:ss");
}
QString clock(int64_t ms) {
    const auto s = std::max<int64_t>(0, ms / 1000);
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
QString person_number(const SearchGroup &g) {
    if (g.person_id.empty())
        return {};
    return QString::fromStdString(g.person_id.substr(g.person_id.rfind('/') + 1));
}
QString camera(const SearchGroup &g) {
    return QString::fromStdString(g.camera_name.empty() ? g.channel : g.camera_name);
}
// A 16:9 video above the zoomed view, so the controls sit directly below the picture.
class VideoBox : public QWidget {
  public:
    VideoBox() {
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override {
        return int(width * 9 / 16 / Tiles::zoom_split);
    }
    QSize sizeHint() const override { return {480, heightForWidth(480)}; }
};
// Labels inside a clickable card must not take its mouse events.
QLabel *passive_label(const QString &text = {}) {
    auto *label = new QLabel(text);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setTextFormat(Qt::PlainText);
    return label;
}
} // namespace
struct SearchPage::Impl {
    SearchPage &page;
    Controller &controller;
    SearchService *service;
    SearchConditions *conditions{};
    QString idle_status{"Enter search conditions."};
    QLabel *status{}, *play_status{}, *player_title{}, *player_time{};
    QWidget *grid_widget{}, *player_panel{}, *container{}, *strip_widget{};
    FlowLayout *grid{};
    QHBoxLayout *strip{};
    QButtonGroup *card_group{}, *strip_group{};
    QPushButton *toggle{};
    QSlider *seek{};
    QSplitter *splitter{};
    QScrollArea *scroll{};
    std::unique_ptr<TimelinePlaybackSession> player;
    Tiles *tiles{};
    std::vector<SearchGroup> groups;
    uint64_t request{}, shown{}, generation{};
    int64_t start{}, end{};
    int active{-1};
    bool interval_active{}, looping{};
    std::string focused;
    std::chrono::steady_clock::time_point reopened, pruned;
    // Widgets of one result card, kept so deletion can update it in place.
    struct CardView {
        QToolButton *card{};
        QLabel *photo{}, *time{}, *score{};
        QProgressBar *bar{};
        int64_t image{};
    };
    std::vector<CardView> views;
    std::string playing_track;
    bool people{};
    struct Pictures {
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> values;
    };
    std::future<Pictures> pictures;
    uint64_t thumbnail_serial{};
    std::map<uint64_t, QPointer<QLabel>> thumbnail_targets;
    std::vector<std::pair<uint64_t, int64_t>> thumbnail_loads;
    explicit Impl(SearchPage &p, Controller &c)
        : page(p), controller(c), service(c.search_service()) {
        p.setObjectName("search-page");
        auto *layout = new QHBoxLayout(&p);
        splitter = new QSplitter;
        splitter->setObjectName("search-splitter");
        splitter->setChildrenCollapsible(false);
        layout->addWidget(splitter);
        auto *left = new QScrollArea;
        left->setWidgetResizable(true);
        left->setMinimumWidth(220);
        conditions = new SearchConditions;
        left->setWidget(conditions);
        splitter->addWidget(left);
        auto *right = new QWidget;
        // One card column plus grid margins and the vertical scroll bar.
        right->setMinimumWidth(card_width + 60);
        auto *right_layout = new QVBoxLayout(right);
        status = new QLabel;
        status->setObjectName("search-status");
        status->setWordWrap(true);
        status->setText("Enter search conditions.");
        right_layout->addWidget(status);
        scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        grid_widget = new QWidget;
        grid_widget->setObjectName("search-results");
        grid = new FlowLayout(grid_widget, 10);
        grid->setContentsMargins(6, 6, 6, 6);
        scroll->setWidget(grid_widget);
        right_layout->addWidget(scroll, 1);
        splitter->addWidget(right);
        card_group = new QButtonGroup(&p);
        strip_group = new QButtonGroup(&p);
        build_player();
        splitter->addWidget(player_panel);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setStretchFactor(2, 1);
        splitter->setSizes({470, 680, 0});
        conditions->invalidate = [this](bool clear) {
            if (service)
                service->cancel();
            request = 0;
            if (clear)
                reset();
            idle_status = conditions->empty() ? "Enter search conditions." : "Searching…";
            status->setText(idle_status);
        };
        conditions->submit = [this] { submit(); };
        conditions->setEnabled(service);
        auto *timer = new QTimer(&p);
        QObject::connect(timer, &QTimer::timeout, &p, [this] { tick(); });
        timer->start(33);
        generation = controller.recording_catalog().deletion_generation();
    }
    ~Impl() {
        if (pictures.valid())
            pictures.wait();
        delete tiles;
        player.reset();
    }
    void build_player() {
        player_panel = new QWidget;
        player_panel->setObjectName("search-player");
        player_panel->setMinimumWidth(400);
        auto *vl = new QVBoxLayout(player_panel);
        player_title = new QLabel;
        player_title->setObjectName("search-player-title");
        player_title->setTextFormat(Qt::PlainText);
        player_title->setWordWrap(true);
        auto font = player_title->font();
        font.setBold(true);
        player_title->setFont(font);
        vl->addWidget(player_title);
        // The renderer window follows this placeholder; see update_video().
        container = new VideoBox;
        container->setObjectName("search-video");
        vl->addWidget(container);
        auto *controls = new QHBoxLayout;
        toggle = new QPushButton("Pause");
        toggle->setObjectName("search-play");
        toggle->setMinimumWidth(70);
        controls->addWidget(toggle);
        seek = new QSlider(Qt::Horizontal);
        seek->setRange(0, 1000);
        controls->addWidget(seek, 1);
        player_time = new QLabel;
        controls->addWidget(player_time);
        vl->addLayout(controls);
        play_status = new QLabel;
        vl->addWidget(play_status);
        auto *strip_scroll = new QScrollArea;
        strip_scroll->setObjectName("search-strip");
        strip_scroll->setWidgetResizable(true);
        strip_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        strip_scroll->setFixedHeight(strip_image_height + 32);
        strip_widget = new QWidget;
        strip = new QHBoxLayout(strip_widget);
        strip->setContentsMargins(2, 2, 2, 2);
        strip->setSpacing(4);
        strip->addStretch();
        strip_scroll->setWidget(strip_widget);
        vl->addWidget(strip_scroll);
        vl->addStretch();
        player_panel->hide();
        QObject::connect(toggle, &QPushButton::clicked, &page, [this] { toggle_play(); });
        QObject::connect(seek, &QSlider::sliderReleased, &page, [this] {
            if (active >= 0 && player)
                player->seek(std::min(end - 1, start + (end - start) * seek->value() / 1000));
        });
    }
    void toggle_play() {
        if (!player || active < 0)
            return;
        auto s = player->status();
        if (looping && s.state != "paused" && s.state != "ended" && s.state != "stopped") {
            player->pause();
            looping = false;
            return;
        }
        looping = interval_active = true;
        if (s.state == "ended" || s.state == "stopped" || s.position_ms >= end)
            restart();
        else
            player->play();
    }
    void restart() {
        player->open_range(groups[size_t(active)].channel, start, end);
        reopened = std::chrono::steady_clock::now();
    }
    void submit() {
        conditions->debounce.stop();
        if (!service || conditions->empty())
            return;
        request = conditions->current_mode() == SearchConditions::Image
                      ? service->image(conditions->image)
                      : service->text(conditions->query_text().toStdString(), negatives(),
                                      alternatives());
        status->setText("Searching…");
    }
    std::vector<std::string> negatives() const { return strings(conditions->negatives()); }
    std::vector<std::string> alternatives() const {
        return strings(conditions->alternatives());
    }
    static std::vector<std::string> strings(const QStringList &list) {
        std::vector<std::string> out;
        for (const auto &text : list)
            out.push_back(text.toStdString());
        return out;
    }
    void load_thumbnail(QLabel *label, int64_t id, QSize size) {
        label->setProperty("thumbnail-size", size);
        const auto previous = label->property("thumbnail-token").toULongLong();
        thumbnail_targets.erase(previous);
        thumbnail_loads.erase(
            std::remove_if(thumbnail_loads.begin(), thumbnail_loads.end(),
                           [previous](auto load) { return load.first == previous; }),
            thumbnail_loads.end());
        const auto token = ++thumbnail_serial;
        label->setProperty("thumbnail-token", QVariant::fromValue<qulonglong>(token));
        thumbnail_targets[token] = label;
        thumbnail_loads.emplace_back(token, id);
    }
    void stop_player() {
        if (player)
            player->stop();
        interval_active = looping = false;
        delete tiles;
        tiles = nullptr;
        focused.clear();
        player_panel->hide();
        active = -1;
        clear_strip();
    }
    void clear_strip() {
        for (auto *b : strip_group->buttons())
            strip_group->removeButton(b);
        while (strip->count() > 1) {
            auto *item = strip->takeAt(0);
            delete item->widget();
            delete item;
        }
    }
    void reset() {
        thumbnail_targets.clear();
        thumbnail_loads.clear();
        stop_player();
        groups.clear();
        build_cards();
        shown = 0;
    }
    // Times of the tracklet that contains `image`; untracked images share one interval.
    static std::pair<int64_t, int64_t> tracklet(const SearchGroup &g, size_t image) {
        const auto &track = g.images[image].track_id;
        std::pair<int64_t, int64_t> span{g.images[image].time_ms, g.images[image].time_ms};
        for (const auto &im : g.images)
            if (im.track_id == track) {
                span.first = std::min(span.first, im.time_ms);
                span.second = std::max(span.second, im.time_ms);
            }
        return span;
    }
    void render(std::vector<SearchGroup> value) {
        reset();
        groups = std::move(value);
        build_cards();
    }
    // Groups emptied by recording deletion keep their index but get no card.
    void build_cards() {
        for (auto *b : card_group->buttons())
            card_group->removeButton(b);
        while (auto *item = grid->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        views.assign(groups.size(), {});
        const double best = best_similarity();
        for (size_t i = 0; i < groups.size(); ++i) {
            const auto &g = groups[i];
            if (g.images.empty())
                continue;
            auto *card = new QToolButton;
            card->setObjectName(QString("search-result-%1").arg(i));
            card->setCheckable(true);
            card->setStyleSheet("QToolButton{border:1px solid palette(mid);border-radius:6px;"
                                "background:palette(base)}"
                                "QToolButton:hover{border-color:#1565c0}"
                                "QToolButton:checked{border:3px solid #1565c0}");
            auto *l = new QVBoxLayout(card);
            l->setContentsMargins(7, 7, 7, 7);
            l->setSpacing(3);
            auto *photo = passive_label();
            photo->setFixedSize(card_image_width, card_image_height);
            photo->setAlignment(Qt::AlignCenter);
            l->addWidget(photo);
            load_thumbnail(photo, g.images.front().id, photo->size());
            auto *name = passive_label(camera(g));
            auto font = name->font();
            font.setBold(true);
            name->setFont(font);
            l->addWidget(name);
            auto *time = passive_label();
            l->addWidget(time);
            auto *score = new QHBoxLayout;
            auto *bar = new QProgressBar;
            bar->setAttribute(Qt::WA_TransparentForMouseEvents);
            bar->setRange(0, 100);
            bar->setTextVisible(false);
            bar->setFixedHeight(6);
            bar->setStyleSheet("QProgressBar{border:none;background:palette(midlight);"
                               "border-radius:3px}"
                               "QProgressBar::chunk{background:#1565c0;border-radius:3px}");
            score->addWidget(bar, 1);
            auto *value = passive_label();
            score->addWidget(value);
            l->addLayout(score);
            views[i] = {card, photo, time, value, bar, g.images.front().id};
            describe(i, best);
            card->setFixedSize(card_width, l->sizeHint().height());
            card_group->addButton(card, int(i));
            QObject::connect(card, &QToolButton::clicked, &page, [this, i] { select(int(i)); });
            grid->addWidget(card);
        }
        if (active >= 0)
            card_group->button(active)->setChecked(true);
    }
    double best_similarity() const {
        double best = 1e-6;
        for (const auto &g : groups)
            if (!g.images.empty())
                best = std::max(best, g.similarity);
        return best;
    }
    // Text and bar of a card; they change when deletion removes some of its images.
    void describe(size_t i, double best) {
        const auto &g = groups[i];
        auto &v = views[i];
        v.time->setText(QString("%1 · %2 s")
                            .arg(short_time(g.from_ms))
                            .arg(std::max<int64_t>(1, (g.to_ms - g.from_ms + 999) / 1000)));
        v.bar->setValue(int(std::clamp(100 * g.similarity / best, 0., 100.)));
        v.score->setText(QString::number(g.similarity, 'f', 2));
    }
    size_t remaining() const {
        return size_t(std::count_if(groups.begin(), groups.end(),
                                    [](const auto &g) { return !g.images.empty(); }));
    }
    void show_count() {
        status->setText(groups.empty() ? QString("No matches")
                        : !remaining() ? QString("The matching recordings were deleted.")
                                       : QString(people ? "%1 people found" : "%1 matches")
                                             .arg(remaining()));
    }
    // Retention and quota deletion run continuously; only drop what was actually deleted.
    void prune() {
        auto &catalog = controller.recording_catalog();
        const auto present = [&catalog](const std::string &name) {
            return catalog.recording_present(name);
        };
        std::vector<size_t> changed;
        for (size_t i = 0; i < groups.size(); ++i)
            if (!groups[i].images.empty() && prune_search_group(groups[i], present))
                changed.push_back(i);
        if (changed.empty())
            return;
        // Update in place: rebuilding every card would reload all thumbnails and
        // flicker each time retention deletes an old segment.
        for (auto i : changed) {
            auto &v = views[i];
            if (groups[i].images.empty()) {
                if (int(i) == active)
                    stop_player();
                card_group->removeButton(v.card);
                grid->removeWidget(v.card);
                v.card->deleteLater();
                v = {};
            } else if (v.image != groups[i].images.front().id) {
                v.image = groups[i].images.front().id;
                load_thumbnail(v.photo, v.image, v.photo->size());
            }
        }
        const double best = best_similarity();
        for (size_t i = 0; i < groups.size(); ++i)
            if (views[i].card)
                describe(i, best);
        if (active >= 0 &&
            std::find(changed.begin(), changed.end(), size_t(active)) != changed.end())
            prune_player();
        show_count();
    }
    // The selected result lost images: drop their strip buttons, and replay only when
    // the tracklet being played is gone.
    void prune_player() {
        const auto &g = groups[size_t(active)];
        auto kept = [&g](qlonglong id) {
            return std::any_of(g.images.begin(), g.images.end(),
                               [id](const auto &im) { return im.id == id; });
        };
        for (auto *b : strip_group->buttons())
            if (!kept(b->property("image-id").toLongLong())) {
                strip_group->removeButton(b);
                strip->removeWidget(b);
                b->deleteLater();
            }
        player_title->setText(title(g));
        if (std::none_of(g.images.begin(), g.images.end(),
                         [this](const auto &im) { return im.track_id == playing_track; }))
            play(active, 0);
    }
    QString title(const SearchGroup &g) const {
        const auto person = person_number(g);
        return QString("%1%2 · %3 – %4 · similarity %5")
            .arg(person.isEmpty() ? "" : "Person " + person + " · ", camera(g),
                 full_time(g.from_ms), short_time(g.to_ms))
            .arg(g.similarity, 0, 'f', 3);
    }
    // Strip buttons carry the image ID; indexes shift when deletion removes images.
    size_t image_index(int index, qlonglong id) const {
        const auto &images = groups[size_t(index)].images;
        for (size_t n = 0; n < images.size(); ++n)
            if (images[n].id == id)
                return n;
        return 0;
    }
    void select(int index) {
        const auto &g = groups[size_t(index)];
        card_group->button(index)->setChecked(true);
        if (active != index) {
            clear_strip();
            for (size_t n = 0; n < g.images.size(); ++n) {
                auto *button = new QToolButton;
                button->setObjectName(QString("search-strip-%1").arg(n));
                button->setCheckable(true);
                button->setStyleSheet("QToolButton{border:1px solid palette(mid)}"
                                      "QToolButton:checked{border:3px solid #1565c0}");
                auto *bl = new QVBoxLayout(button);
                bl->setContentsMargins(3, 3, 3, 3);
                auto *photo = passive_label();
                photo->setFixedSize(strip_image_width, strip_image_height);
                photo->setAlignment(Qt::AlignCenter);
                bl->addWidget(photo);
                button->setFixedSize(strip_image_width + 6, strip_image_height + 6);
                load_thumbnail(photo, g.images[n].id, photo->size());
                button->setProperty("image-id", qlonglong(g.images[n].id));
                strip_group->addButton(button);
                strip->insertWidget(strip->count() - 1, button);
                QObject::connect(button, &QToolButton::clicked, &page, [this, index, button] {
                    play(index, image_index(index, button->property("image-id").toLongLong()));
                });
            }
            player_title->setText(title(g));
        }
        if (!player_panel->isVisible()) {
            player_panel->show();
            auto sizes = splitter->sizes();
            const int share = std::clamp((sizes[1] + sizes[2]) * 55 / 100, 440,
                                         std::max(440, sizes[1] + sizes[2] - card_width - 60));
            splitter->setSizes({sizes[0], sizes[1] + sizes[2] - share, share});
        }
        play(index, 0);
    }
    void play(int index, size_t image) {
        const auto &g = groups[size_t(index)];
        if (!player)
            player = std::make_unique<TimelinePlaybackSession>(controller.display(),
                                                               controller.recording_catalog());
        if (!tiles) {
            tiles = new Tiles(controller, &player->slot());
            // Use the same native parent strategy as the timeline preview.
            // Qt 6.4 Wayland does not expose dynamically embedded windows in a scroll area.
            page.window()->winId();
            tiles->setParent(page.window()->windowHandle());
            tiles->setObjectName("search-video-renderer");
            tiles->object_labels = false;
            tiles->zoom_view = tiles->immediate_click = true;
            tiles->activated = [this](unsigned, bool twice) {
                if (!twice)
                    toggle_play();
            };
        }
        if (focused != g.person_id) {
            focused = g.person_id;
            focus(g);
        }
        // Loop exactly the tracklet; shorter ones are padded evenly to five seconds.
        const auto span = tracklet(g, image);
        constexpr int64_t shortest = 5000;
        start = span.first;
        end = span.second;
        if (end - start < shortest) {
            start = std::max<int64_t>(0, start - (shortest - (end - start)) / 2);
            end = start + shortest;
        }
        for (auto *b : strip_group->buttons())
            if (b->property("image-id").toLongLong() == g.images[image].id)
                b->setChecked(true);
        playing_track = g.images[image].track_id;
        active = index;
        interval_active = looping = true;
        restart();
        QTimer::singleShot(0, &page, [this] { update_video(); });
    }
    // Person IDs are "run/channel/session/person"; the run never contains '/'.
    void focus(const SearchGroup &g) {
        const auto &id = g.person_id;
        const auto run = id.find('/'), last = id.rfind('/');
        const auto prefix =
            id.substr(0, run == std::string::npos ? 0 : run) + "/" + g.channel + "/";
        const auto person = last == std::string::npos ? std::string() : id.substr(last + 1);
        if (id.compare(0, prefix.size(), prefix) != 0 || last <= prefix.size() ||
            person.empty() || person.find_first_not_of("0123456789") != std::string::npos) {
            tiles->focus({}, {}, 0);
            return;
        }
        tiles->focus(id.substr(0, run), id.substr(prefix.size(), last - prefix.size()),
                     std::stoull(person));
    }
    void update_video() {
        if (!tiles)
            return;
        auto *top = page.window();
        QRect rect(container->mapTo(top, QPoint()), container->size());
        const bool visible = page.isVisible() && player_panel->isVisible() &&
                             rect.width() > 1 && rect.height() > 1;
        tiles->setGeometry(rect);
        tiles->setVisible(visible);
        if (visible)
            tiles->update();
    }
    void tick() {
        update_video();
        if (!service) {
            status->setText("Search is disabled. Configure an embedding model to enable it.");
            return;
        }
        if (pictures.valid() &&
            pictures.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                for (auto &[token, png] : pictures.get().values) {
                    auto target = thumbnail_targets.find(token);
                    if (target == thumbnail_targets.end())
                        continue;
                    auto label = target->second;
                    thumbnail_targets.erase(target);
                    if (!label)
                        continue;
                    QPixmap image;
                    image.loadFromData(png.data(), int(png.size()), "PNG");
                    label->setPixmap(image.scaled(label->property("thumbnail-size").toSize(),
                                                  Qt::KeepAspectRatio, Qt::SmoothTransformation));
                }
            } catch (const std::exception &e) {
                status->setText(e.what());
            }
        }
        if (!pictures.valid() && !thumbnail_loads.empty()) {
            std::vector<std::pair<uint64_t, int64_t>> loads;
            while (!thumbnail_loads.empty() && loads.size() < 8) {
                auto load = thumbnail_loads.front();
                thumbnail_loads.erase(thumbnail_loads.begin());
                if (thumbnail_targets.count(load.first))
                    loads.push_back(load);
            }
            auto *index = &controller.search_index();
            pictures = std::async(std::launch::async, [index, loads = std::move(loads)] {
                Pictures out;
                for (auto [token, id] : loads)
                    out.values.emplace_back(token, index->image(id));
                return out;
            });
        }
        SearchResult r;
        if (request && shown != request)
            r = service->result();
        if (request && r.request == request && shown != request) {
            if (r.error.empty())
                render(std::move(r.groups));
            else
                reset();
            shown = request;
            people = conditions->current_mode() == SearchConditions::Person;
            if (r.error.empty())
                show_count();
            else
                status->setText(QString::fromStdString(r.error));
        }
        auto s = service->status();
        if (!request)
            status->setText(s.state == "ready" ? idle_status
                                               : QString::fromStdString(s.state + ": " + s.error));
        auto p = player ? player->status() : TimelinePlaybackStatus{};
        if (tiles)
            tiles->paused(p.state == "paused" || p.state == "ended");
        toggle->setText(p.state == "playing" || p.state == "waiting" ? "Pause" : "Play");
        if (interval_active) {
            if (looping && (p.state == "ended" || p.position_ms >= end) &&
                std::chrono::steady_clock::now() - reopened > std::chrono::milliseconds(500))
                restart();
            if (!seek->isSliderDown())
                seek->setValue(int(std::clamp<int64_t>(
                    (p.position_ms - start) * 1000 / std::max<int64_t>(1, end - start), 0, 1000)));
            player_time->setText(clock(std::clamp(p.position_ms, start, end) - start) + " / " +
                                 clock(end - start));
            play_status->setText(QString::fromStdString(tiles && !tiles->error().empty()
                                                            ? tiles->error()
                                                        : p.error.empty() ? p.notice
                                                                          : p.error));
        }
        const auto gen = controller.recording_catalog().deletion_generation();
        const auto now = std::chrono::steady_clock::now();
        if (gen != generation && now - pruned >= std::chrono::seconds(1)) {
            generation = gen;
            pruned = now;
            prune();
        }
    }
};
SearchPage::SearchPage(Controller &c, QWidget *p)
    : QWidget(p), impl_(std::make_unique<Impl>(*this, c)) {}
SearchPage::~SearchPage() = default;
void SearchPage::search_image(SearchImage im) {
    auto &p = *impl_;
    if (!p.service)
        return;
    p.conditions->debounce.stop();
    p.conditions->image = std::move(im);
    const auto &source = p.conditions->image;
    QImage image(source.rgb.data(), source.width, source.height, source.width * 3,
                 QImage::Format_RGB888);
    p.conditions->preview->setPixmap(
        QPixmap::fromImage(image).scaled(384, 288, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (p.conditions->current_mode() != SearchConditions::Image)
        p.conditions->set_mode(SearchConditions::Image);
    else
        p.conditions->changed(true, true);
}
streaming::TimelinePlaybackStatus SearchPage::playback_status() const {
    return impl_->player ? impl_->player->status() : TimelinePlaybackStatus{};
}
std::string SearchPage::renderer_status() const {
    auto *t = impl_->tiles;
    if (!t)
        return "no renderer";
    return t->error() + " objects=" + std::to_string(t->displayed_analysis_objects()) +
           " visible=" + std::to_string(t->isVisible()) +
           " exposed=" + std::to_string(t->isExposed()) + " size=" + std::to_string(t->width()) +
           "x" + std::to_string(t->height()) +
           " pos=" + std::to_string(impl_->container->mapToGlobal(QPoint()).x()) + "," +
           std::to_string(impl_->container->mapToGlobal(QPoint()).y());
}
QImage SearchPage::grab_video() const {
    return impl_->tiles ? impl_->tiles->grabFramebuffer() : QImage();
}
uint64_t SearchPage::presented() const { return impl_->tiles ? impl_->tiles->presented() : 0; }
void SearchPage::leave() {
    impl_->conditions->debounce.stop();
    impl_->idle_status = "Change conditions to search.";
    if (impl_->tiles)
        impl_->tiles->hide();
    if (impl_->player)
        impl_->player->stop();
    impl_->interval_active = impl_->looping = false;
    if (impl_->service && impl_->request != impl_->shown) {
        impl_->service->cancel();
        impl_->request = 0;
    }
}
std::future<void> SearchPage::release() {
    leave();
    if (impl_->player)
        return impl_->player->stop_and_release();
    std::promise<void> released;
    released.set_value();
    return released.get_future();
}
} // namespace amdva::nvr
