#include "search_page.hpp"
#include "window.hpp"
#include <QComboBox>
#include <QDateTime>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
namespace amdva::nvr {
using namespace amdva::streaming;
struct SearchPage::Impl {
    SearchPage &page;
    Controller &controller;
    SearchService *service;
    QLineEdit *input{};
    QLabel *status{}, *query_image{}, *play_status{};
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
        std::vector<std::pair<QLabel *, std::vector<uint8_t>>> values;
    };
    std::future<Pictures> pictures;
    explicit Impl(SearchPage &p, Controller &c)
        : page(p), controller(c), service(c.search_service()) {
        p.setObjectName("search-page");
        auto *layout = new QVBoxLayout(&p);
        auto *row = new QHBoxLayout;
        input = new QLineEdit;
        input->setObjectName("search-text");
        input->setPlaceholderText("Describe a person, clothing or appearance…");
        auto *button = new QPushButton("Search");
        button->setObjectName("search-submit");
        row->addWidget(input, 1);
        row->addWidget(button);
        layout->addLayout(row);
        query_image = new QLabel;
        query_image->setObjectName("search-query-image");
        layout->addWidget(query_image);
        status = new QLabel;
        status->setObjectName("search-status");
        status->setWordWrap(true);
        layout->addWidget(status);
        scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        cards = new QWidget;
        results = new QVBoxLayout(cards);
        results->addStretch();
        scroll->setWidget(cards);
        layout->addWidget(scroll, 1);
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
        QObject::connect(button, &QPushButton::clicked, &p, [this] {
            if (!service)
                return;
            reset();
            query_image->clear();
            request = service->text(input->text().trimmed().toStdString());
            status->setText("Searching…");
        });
        QObject::connect(input, &QLineEdit::returnPressed, button, &QPushButton::click);
        button->setEnabled(service);
        input->setEnabled(service);
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
    void reset() {
        if (pictures.valid())
            pictures.wait();
        pictures = {};
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
        std::vector<std::pair<QLabel *, int64_t>> loads;
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
            title->setTextFormat(Qt::PlainText);
            l->addWidget(title);
            auto *thumbs = new QHBoxLayout;
            auto *photo = new QLabel;
            photo->setMinimumSize(100, 120);
            thumbs->addWidget(photo);
            loads.emplace_back(photo, g.images.front().id);
            for (size_t n = 1; n < std::min<size_t>(4, g.images.size()); ++n) {
                auto *extra = new QLabel;
                thumbs->addWidget(extra);
                loads.emplace_back(extra, g.images[n].id);
            }
            auto *choose = new QComboBox;
            for (const auto &im : g.images)
                choose->addItem(time(im.time_ms), QVariant::fromValue<qlonglong>(im.id));
            thumbs->addWidget(choose);
            thumbs->addStretch();
            l->addLayout(thumbs);
            QObject::connect(
                choose, qOverload<int>(&QComboBox::activated), &page, [this, choose, photo](int) {
                    // Keep only one bounded thumbnail read job; no UI-thread SQLite work.
                    if (pictures.valid())
                        return;
                    auto id = choose->currentData().toLongLong();
                    pictures = std::async(std::launch::async, [this, photo, id] {
                        Pictures out;
                        out.values.emplace_back(
                            photo, controller.recording_catalog().observation_image(id));
                        return out;
                    });
                });
            auto *play = new QPushButton("Play nearby video");
            l->addWidget(play);
            QObject::connect(play, &QPushButton::clicked, &page, [this, card, i] {
                if (!player)
                    player = std::make_unique<TimelinePlaybackSession>(
                        controller.display(), controller.recording_catalog());
                const auto &g = groups[i];
                start = std::max<int64_t>(0, g.from_ms - 5000);
                end = g.to_ms + 5000;
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
        pictures = std::async(std::launch::async, [this, loads] {
            Pictures out;
            for (auto [label, id] : loads)
                out.values.emplace_back(label,
                                        controller.recording_catalog().observation_image(id));
            return out;
        });
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
            status->setText(
                "Search is disabled. Configure an EmbeddingGemma 2 bundle to enable it.");
            return;
        }
        if (pictures.valid() &&
            pictures.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                for (auto &[label, png] : pictures.get().values) {
                    QPixmap image;
                    image.loadFromData(png.data(), int(png.size()), "PNG");
                    label->setPixmap(
                        image.scaled(192, 144, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                }
            } catch (const std::exception &e) {
                status->setText(e.what());
            }
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
            status->setText(s.state == "ready"
                                ? QString("Ready · %1 images stored · %2 skipped · %3 queued")
                                      .arg(s.stored)
                                      .arg(s.dropped)
                                      .arg(s.pending)
                                : QString::fromStdString(s.state + ": " + s.error));
        auto p = player ? player->status() : TimelinePlaybackStatus{};
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
            service->cancel();
            request = 0;
            reset();
            status->setText("Recordings changed. Search again to refresh results.");
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
    p.reset();
    p.input->clear();
    QImage image(im.rgb.data(), im.width, im.height, im.width * 3, QImage::Format_RGB888);
    p.query_image->setPixmap(QPixmap::fromImage(image).scaled(192, 144, Qt::KeepAspectRatio));
    p.request = p.service->image(std::move(im));
    p.status->setText("Searching by selected region…");
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
