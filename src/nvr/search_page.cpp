#include "search_page.hpp"
#include "search_conditions.hpp"
#include "window.hpp"
#include <QComboBox>
#include <QDateTime>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
namespace amdva::nvr {
using namespace amdva::streaming;
struct SearchPage::Impl {
    SearchPage &page;
    Controller &controller;
    SearchService *service;
    SearchConditions *conditions{};
    QString idle_status{"Enter search conditions."};
    QLabel *status{}, *play_status{};
    QWidget *cards{}, *video{}, *active_card{}, *container{};
    QVBoxLayout *results{};
    QSlider *seek{};
    QScrollArea *scroll{};
    std::unique_ptr<TimelinePlaybackSession> player;
    Tiles *tiles{};
    std::vector<SearchGroup> groups;
    uint64_t request{}, shown{}, generation{};
    int64_t start{}, end{};
    bool interval_active{};
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
        auto *splitter = new QSplitter;
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
        right->setMinimumWidth(280);
        auto *right_layout = new QVBoxLayout(right);
        status = new QLabel;
        status->setObjectName("search-status");
        status->setWordWrap(true);
        status->setText("Enter search conditions.");
        right_layout->addWidget(status);
        scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        cards = new QWidget;
        results = new QVBoxLayout(cards);
        results->addStretch();
        scroll->setWidget(cards);
        right_layout->addWidget(scroll, 1);
        splitter->addWidget(right);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({320, 680});
        video = new QWidget(&p);
        auto *vl = new QVBoxLayout(video);
        auto *controls = new QHBoxLayout;
        auto *play = new QPushButton("Play"), *pause = new QPushButton("Pause");
        controls->addWidget(play);
        controls->addWidget(pause);
        seek = new QSlider(Qt::Horizontal);
        seek->setRange(0, 1000);
        controls->addWidget(seek, 1);
        vl->addLayout(controls);
        play_status = new QLabel;
        vl->addWidget(play_status);
        video->hide();
        QObject::connect(play, &QPushButton::clicked, &p, [this] {
            if (active_card && player) {
                auto s = player->status();
                if (s.state == "ended" || s.state == "stopped" || s.position_ms >= end)
                    player->open_range(s.channel, start, end);
                else
                    player->play();
                interval_active = true;
            }
        });
        QObject::connect(pause, &QPushButton::clicked, &p, [this] {
            if (player)
                player->pause();
        });
        QObject::connect(seek, &QSlider::sliderReleased, &p, [this] {
            if (active_card && player)
                player->seek(std::min(end - 1, start + (end - start) * seek->value() / 1000));
        });
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
        delete video;
        player.reset();
    }
    void submit() {
        conditions->debounce.stop();
        if (!service || conditions->empty())
            return;
        request = conditions->mode->currentIndex() == SearchConditions::Image
                      ? service->image(conditions->image)
                      : service->text(conditions->text().toStdString());
        status->setText("Searching…");
    }
    void load_thumbnail(QLabel *label, int64_t id) {
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
    void reset() {
        thumbnail_targets.clear();
        thumbnail_loads.clear();
        if (player)
            player->stop();
        interval_active = false;
        delete tiles;
        tiles = nullptr;
        delete container;
        container = nullptr;
        video->hide();
        video->setParent(&page);
        active_card = nullptr;
        while (results->count() > 1) {
            auto *item = results->takeAt(0);
            delete item->widget();
            delete item;
        }
        groups.clear();
        shown = 0;
    }
    void render(std::vector<SearchGroup> value) {
        reset();
        groups = std::move(value);

        for (size_t i = 0; i < groups.size(); ++i) {
            const auto &g = groups[i];
            auto *card = new QWidget;
            card->setObjectName(QString("search-result-%1").arg(i));
            auto *l = new QVBoxLayout(card);
            auto time = [](int64_t t) {
                return QDateTime::fromMSecsSinceEpoch(t).toString("yyyy-MM-dd HH:mm:ss");
            };
            auto *title = new QLabel(
                QString("%1 · %2 – %3 · similarity %4 · %5 images")
                    .arg(QString::fromStdString(g.camera_name.empty() ? g.channel : g.camera_name),
                         time(g.from_ms), time(g.to_ms))
                    .arg(g.similarity, 0, 'f', 3)
                    .arg(g.images.size()));
            if (!g.person_id.empty())
                title->setText(
                    "Person " +
                    QString::fromStdString(g.person_id.substr(g.person_id.rfind('/') + 1)) + " · " +
                    title->text());
            title->setTextFormat(Qt::PlainText);
            title->setWordWrap(true);
            l->addWidget(title);
            auto *thumbs = new QHBoxLayout;
            auto *photo = new QLabel;
            photo->setMinimumSize(64, 96);
            photo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            thumbs->addWidget(photo, 1);
            load_thumbnail(photo, g.images.front().id);
            for (size_t n = 1; n < std::min<size_t>(4, g.images.size()); ++n) {
                auto *extra = new QLabel;
                extra->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                thumbs->addWidget(extra, 1);
                load_thumbnail(extra, g.images[n].id);
            }
            auto *choose = new QComboBox;
            for (const auto &im : g.images)
                choose->addItem(time(im.time_ms), QVariant::fromValue<qlonglong>(im.id));

            l->addLayout(thumbs);
            l->addWidget(choose);
            QObject::connect(choose, qOverload<int>(&QComboBox::activated), &page,
                             [this, choose, photo](int) {
                                 load_thumbnail(photo, choose->currentData().toLongLong());
                             });
            auto *appearances = new QComboBox;
            std::map<std::string, std::pair<int64_t, int64_t>> intervals;
            for (const auto &im : g.images) {
                auto key = im.track_id.empty() ? std::string("legacy") : im.track_id;
                auto it = intervals.find(key);
                if (it == intervals.end())
                    intervals[key] = {im.time_ms, im.time_ms};
                else {
                    it->second.first = std::min(it->second.first, im.time_ms);
                    it->second.second = std::max(it->second.second, im.time_ms);
                }
            }
            for (const auto &[id, span] : intervals) {
                appearances->addItem(time(span.first) + " – " + time(span.second),
                                     QVariant::fromValue<qlonglong>(span.first));
                appearances->setItemData(appearances->count() - 1,
                                         QVariant::fromValue<qlonglong>(span.second),
                                         Qt::UserRole + 1);
            }
            l->addWidget(appearances);
            auto *play = new QPushButton("Play nearby video");
            l->addWidget(play);
            QObject::connect(play, &QPushButton::clicked, &page, [this, card, i, appearances] {
                if (!player)
                    player = std::make_unique<TimelinePlaybackSession>(
                        controller.display(), controller.recording_catalog());
                const auto &g = groups[i];
                start = std::max<int64_t>(0, appearances->currentData().toLongLong() - 5000);
                end = appearances->currentData(Qt::UserRole + 1).toLongLong() + 5000;
                if (active_card != card) {
                    delete tiles;
                    tiles = nullptr;
                    delete container;
                    container = nullptr;
                    video->hide();
                    video->setParent(card);
                    card->layout()->addWidget(video);
                    tiles = new Tiles(controller, &player->slot());
                    // Use the same native parent strategy as the timeline preview.
                    // Qt 6.4 Wayland does not expose dynamically embedded windows in a scroll area.
                    page.window()->winId();
                    tiles->setParent(page.window()->windowHandle());
                    container = new QWidget(video);
                    container->setMinimumHeight(240);
                    static_cast<QVBoxLayout *>(video->layout())->insertWidget(0, container);
                }
                video->show();
                container->show();
                QTimer::singleShot(0, &page, [this] {
                    if (container) {
                        scroll->ensureWidgetVisible(container, 0, 0);
                        update_video();
                    }
                });
                active_card = card;
                interval_active = true;
                player->open_range(g.channel, start, end);
            });
            results->insertWidget(results->count() - 1, card);
        }
    }
    void update_video() {
        if (!tiles || !container)
            return;
        auto *top = page.window();
        QRect rect(container->mapTo(top, QPoint()), container->size());
        QRect clip(scroll->viewport()->mapTo(top, QPoint()), scroll->viewport()->size());
        const QRect visible_rect = rect.intersected(clip);
        const bool visible = page.isVisible() && video->isVisible() && visible_rect.width() > 1 &&
                             visible_rect.height() > 1;
        tiles->setGeometry(visible_rect);
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
                    label->setPixmap(
                        image.scaled(144, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
            auto *catalog = &controller.recording_catalog();
            pictures = std::async(std::launch::async, [catalog, loads = std::move(loads)] {
                Pictures out;
                for (auto [token, id] : loads)
                    out.values.emplace_back(token, catalog->observation_image(id));
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
            status->setText(r.error.empty() ? QString("%1 result groups").arg(groups.size())
                                            : QString::fromStdString(r.error));
        }
        auto s = service->status();
        page.setToolTip(QString("%1 · stored %2 · skipped %3 · queued %4 · %5 ms\n%6")
                            .arg(QString::fromStdString(s.state))
                            .arg(s.stored)
                            .arg(s.dropped)
                            .arg(s.pending)
                            .arg(s.inference_ms, 0, 'f', 1)
                            .arg(QString::fromStdString(s.error)));
        if (!request)
            status->setText(s.state == "ready" ? idle_status
                                               : QString::fromStdString(s.state + ": " + s.error));
        auto p = player ? player->status() : TimelinePlaybackStatus{};
        if (tiles)
            tiles->paused(p.state == "paused" || p.state == "ended");
        if (interval_active) {
            if (p.position_ms >= end) {
                player->pause();
                interval_active = false;
            }
            if (!seek->isSliderDown())
                seek->setValue(int(std::clamp<int64_t>(
                    (p.position_ms - start) * 1000 / std::max<int64_t>(1, end - start), 0, 1000)));
            play_status->setText(QString::fromStdString(tiles && !tiles->error().empty()
                                                            ? tiles->error()
                                                        : p.error.empty() ? p.state + " " + p.notice
                                                                          : p.error));
        }
        auto gen = controller.recording_catalog().deletion_generation();
        if (gen != generation) {
            generation = gen;
            conditions->debounce.stop();
            service->cancel();
            request = 0;
            reset();
            idle_status = "Recordings changed. Change conditions to refresh results.";
            status->setText(idle_status);
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
        QPixmap::fromImage(image).scaled(256, 192, Qt::KeepAspectRatio));
    if (p.conditions->mode->currentIndex() != SearchConditions::Image)
        p.conditions->mode->setCurrentIndex(SearchConditions::Image);
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
    return t->error() + " visible=" + std::to_string(t->isVisible()) +
           " exposed=" + std::to_string(t->isExposed()) + " size=" + std::to_string(t->width()) +
           "x" + std::to_string(t->height()) +
           " pos=" + std::to_string(impl_->container->mapToGlobal(QPoint()).x()) + "," +
           std::to_string(impl_->container->mapToGlobal(QPoint()).y());
}
uint64_t SearchPage::presented() const { return impl_->tiles ? impl_->tiles->presented() : 0; }
void SearchPage::leave() {
    impl_->conditions->debounce.stop();
    impl_->idle_status = "Change conditions to search.";
    if (impl_->tiles)
        impl_->tiles->hide();
    if (impl_->player)
        impl_->player->stop();
    impl_->interval_active = false;
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
