#include "timeline.hpp"
#include "search_page.hpp"
#include "window.hpp"
#include <QApplication>
#include <QAbstractButton>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTimer>
#include <dlfcn.h>
#include <iostream>
#include <map>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 2 && (argc != 3 || std::string(argv[2]) != "--exercise-deletion")) {
        std::cerr << "Usage: amdva-search-validate CONFIG [--exercise-deletion]\n";
        return 2;
    }
    initialize_runtime();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    int failed = 0;
    try {
        auto options = load_config(argv[1]);
        Controller controller(options);
        Window window(controller, options, argv[1]);
        window.resize(1000, 800);
        window.show();
        auto *service = controller.search_service();
        if (!service)
            throw std::runtime_error("Search disabled");
        auto *tabs = window.findChild<QTabWidget *>("vms-tabs");
        const int search_tab = tabs->indexOf(window.findChild<QWidget *>("search-page"));
        auto *input = window.findChild<QLineEdit *>("search-text");

        auto audit = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        bool auditing = false;
        unsigned live_drag_checks = 0;
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        QTimer timer;
        int tick = 0, stage = 0, region_tick = 0, strip_tick = 0;
        int64_t loop_peak = 0;
        bool exiting = false;
        int unrelated_tick = 0, unrelated_cards = 0;
        QPointF drag_end;
        auto capture_live = [&](const char *name) {
            if (qEnvironmentVariableIsSet("AMDVA_VALIDATE_CAPTURE"))
                window.live_view()->grabFramebuffer().save(QString::fromStdString(
                    (std::filesystem::path(argv[1]).parent_path() / name).string()));
        };
        auto click_video = [&] {
            auto *video = window.windowHandle()->findChild<QWindow *>("search-video-renderer");
            if (!video)
                throw std::runtime_error("Missing search video renderer");
            const QPointF at(video->width() / 2., video->height() / 4.);
            QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(video, &press);
            QApplication::sendEvent(video, &release);
        };
        bool region_logged = false;
        uint64_t request = 0;
        int64_t last = 0, observation_id = 0;
        int deletion_start = 0;
        QObject::connect(&timer, &QTimer::timeout, &app, [&] {
            try {
                ++tick;
                auto state = service->status();
                if (!auditing && controller.inference_ready() && state.state == "ready") {
                    if (audit)
                        audit();
                    auditing = true;
                }
                if (tick % 10 == 0)
                    std::cout << "stage=" << stage << " state=" << state.state
                              << " stored=" << state.stored << " dropped=" << state.dropped
                              << " pending=" << state.pending << " ms=" << state.inference_ms
                              << " error=" << state.error << std::endl;
                if (state.state == "error")
                    throw std::runtime_error(state.error);
                if (tick > 600)
                    throw std::runtime_error("Validation timed out");
                if (stage == 0 && tick >= 75 && state.stored >= 6 &&
                    controller.recordings().size() >= 2) {
                    if (qEnvironmentVariableIsSet("AMDVA_VALIDATE_CAPTURE"))
                        window.live_view()->grabFramebuffer().save(QString::fromStdString(
                            (std::filesystem::path(argv[1]).parent_path() / "person-overlay.png")
                                .string()));
                    tabs->setCurrentIndex(search_tab);
                    auto *mode = window.findChild<QPushButton *>("search-mode-person");
                    if (!mode)
                        throw std::runtime_error("Missing search modes");
                    mode->click();
                    window.findChild<QAbstractButton *>("search-person-lower-3")->click();
                    window.findChild<QAbstractButton *>("search-person-lower-color-10")->click();
                    stage = 9;
                } else if (stage == 9) {
                    auto r = service->result();
                    if (!r.error.empty())
                        throw std::runtime_error(r.error);
                    if (r.request && !r.groups.empty() &&
                        window.findChild<QWidget *>("search-result-0")) {
                        std::cout << "PASS: checkbox person search; groups=" << r.groups.size()
                                  << " best=" << r.groups.front().similarity << std::endl;
                        window.grab().save(QString::fromStdString(
                            (std::filesystem::path(argv[1]).parent_path() / "person-search.png")
                                .string()));
                        input->setText("a person wearing a purple skirt");
                        window.findChild<QPushButton *>("search-mode-text")->click();
                        // Exercise debounce rather than mode-switch submission.
                        input->setText("a person wearing a purple skirt outdoors");
                        stage = 1;
                    }
                } else if (stage == 1) {
                    auto r = service->result();
                    if (!r.error.empty())
                        throw std::runtime_error(r.error);
                    if (r.request && !r.groups.empty()) {
                        std::cout << "English groups=" << r.groups.size()
                                  << " best=" << r.groups.front().similarity << std::endl;
                        if (!options.search.reid_model.empty()) {
                            for (const auto &g : r.groups) {
                                if (g.person_id.empty())
                                    throw std::runtime_error("Missing person ID");
                                for (const auto &im : g.images)
                                    if (im.person_id != g.person_id || im.channel != g.channel ||
                                        im.track_id.empty())
                                        throw std::runtime_error("Mixed person identities");
                            }
                            std::cout << "PASS: person cards contain a single camera/person and "
                                         "tracklet IDs"
                                      << std::endl;
                        }
                        observation_id = r.groups.front().images.front().id;
                        auto png = controller.search_index().image(observation_id);
                        QFile picture(QString::fromStdString(
                            (std::filesystem::path(argv[1]).parent_path() / "english-match.png")
                                .string()));
                        if (picture.open(QIODevice::WriteOnly))
                            picture.write(reinterpret_cast<const char *>(png.data()), png.size());
                        request = r.request;
                        input->setText(QString::fromUtf8("紫色のスカートを着た人"));

                        stage = 2;
                    }
                } else if (stage == 2) {
                    auto r = service->result();
                    if (r.request > request && !r.groups.empty()) {
                        std::cout << "Japanese groups=" << r.groups.size()
                                  << " best=" << r.groups.front().similarity << std::endl;
                        request = r.request;
                        last = r.groups.front().from_ms;
                        unrelated_tick = tick;
                        stage = 16;
                    }
                } else if (stage == 16 || stage == 17) {
                    // Retention deletes old recordings all the time; the shown results must
                    // only lose the cards whose images were all in the deleted recording.
                    const auto text = window.findChild<QLabel *>("search-status")->text();
                    int cards = 0;
                    for (auto *b : window.findChildren<QAbstractButton *>())
                        cards += b->objectName().startsWith("search-result-");
                    if (stage == 16 && cards) {
                        const auto groups = service->result().groups;
                        std::map<std::string, size_t> used;
                        for (const auto &g : groups)
                            for (const auto &im : g.images)
                                used[im.recording]++;
                        // Prefer a recording outside the results, else the least used one.
                        std::string victim;
                        size_t fewest = SIZE_MAX;
                        for (const auto &rec : controller.recordings())
                            if (rec.state == "finalized" && used[rec.name] < fewest) {
                                victim = rec.name;
                                fewest = used[rec.name];
                            }
                        if (victim.empty())
                            throw std::runtime_error("No finalized recording to delete");
                        unrelated_cards = 0;
                        for (const auto &g : groups)
                            unrelated_cards += std::any_of(
                                g.images.begin(), g.images.end(),
                                [&](const auto &im) { return im.recording != victim; });
                        if (!unrelated_cards)
                            throw std::runtime_error("Every result is in one recording");
                        DeletionJob job;
                        job.pending.push_back(victim);
                        job.total = 1;
                        controller.recording_catalog().delete_batch(job);
                        if (job.deleted != 1)
                            throw std::runtime_error("Could not delete " + victim);
                        std::cout << "Deleted " << victim << " used by " << fewest
                                  << " result images; " << cards << " cards" << std::endl;
                        unrelated_tick = tick;
                        stage = 17;
                    } else if (stage == 17 && tick > unrelated_tick + 8) {
                        if (cards != unrelated_cards ||
                            !text.startsWith(QString::number(cards) + " "))
                            throw std::runtime_error("Deletion changed results: " +
                                                     text.toStdString() + " cards=" +
                                                     std::to_string(cards));
                        std::cout << "PASS: retention-style deletion kept " << cards << " results"
                                  << std::endl;
                        stage = 3;
                    } else if (tick > unrelated_tick + 50)
                        throw std::runtime_error("Results did not appear");
                } else if (stage == 3) {
                    // Prefer a result with several images so the strip can be exercised.
                    // Cards removed by the deletion above keep their result index.
                    const auto groups = service->result().groups;
                    QAbstractButton *card = nullptr;
                    for (size_t i = 0; i < groups.size(); ++i)
                        if (auto *c = window.findChild<QAbstractButton *>(
                                QString("search-result-%1").arg(i))) {
                            if (!card || groups[i].images.size() >= 2)
                                card = c;
                            if (groups[i].images.size() >= 2)
                                break;
                        }
                    if (card) {
                        card->click();
                        if (!card->isChecked() ||
                            !window.findChild<QWidget *>("search-player")->isVisible())
                            throw std::runtime_error("Selecting a result did not open the player");
                        stage = 4;
                    }
                } else if (stage == 4) {
                    auto *page =
                        dynamic_cast<SearchPage *>(window.findChild<QWidget *>("search-page"));
                    if (page && tick % 10 == 0) {
                        auto p = page->playback_status();
                        window.grab().save(QString::fromStdString(
                            (std::filesystem::path(argv[1]).parent_path() / "search-ui.png")
                                .string()));
                        std::cout << "Playback " << p.state << " frames=" << p.frames
                                  << " at=" << p.position_ms << " error=" << p.error
                                  << " notice=" << p.notice
                                  << " renderer=" << page->renderer_status()
                                  << " presented=" << page->presented() << std::endl;
                    }
                    if (page && page->playback_status().frames > 3 && page->presented() > 3) {
                        std::cout << "Card frames=" << page->playback_status().frames << std::endl;
                        // A second strip image replays from its own tracklet.
                        if (auto *image = window.findChild<QAbstractButton *>("search-strip-1")) {
                            image->click();
                            if (!image->isChecked())
                                throw std::runtime_error("Strip image was not selected");
                            strip_tick = tick;
                            stage = 10;
                            return;
                        }
                        stage = 11;
                    }
                } else if (stage == 10) {
                    auto *page =
                        dynamic_cast<SearchPage *>(window.findChild<QWidget *>("search-page"));
                    auto p = page->playback_status();
                    if (!p.error.empty())
                        throw std::runtime_error("Strip playback: " + p.error);
                    // Capture once the person's box is on screen (or give up waiting).
                    const bool person = page->renderer_status().find(" objects=0 ") ==
                                        std::string::npos;
                    if (tick > strip_tick + 5 && p.frames > 3 && page->presented() > 3 &&
                        (person || tick > strip_tick + 25)) {
                        std::cout << "PASS: strip image playback frames=" << p.frames << std::endl;
                        page->grab_video().save(QString::fromStdString(
                            (std::filesystem::path(argv[1]).parent_path() / "search-video.png")
                                .string()));
                        click_video();
                        strip_tick = tick;
                        stage = 12;
                    }
                } else if (stage == 12 || stage == 13) {
                    // Clicking the video pauses, and a second click resumes.
                    auto *page =
                        dynamic_cast<SearchPage *>(window.findChild<QWidget *>("search-page"));
                    const auto state = page->playback_status().state;
                    const bool want_paused = stage == 12;
                    if ((state == "paused") == want_paused) {
                        std::cout << "PASS: video click " << (want_paused ? "paused" : "resumed")
                                  << std::endl;
                        if (stage == 12)
                            click_video();
                        strip_tick = tick;
                        stage = stage == 12 ? 13 : 14;
                    } else if (tick > strip_tick + 15) {
                        throw std::runtime_error("Video click did not toggle playback: " + state);
                    }
                } else if (stage == 14) {
                    // Playback loops over the tracklet instead of stopping at its end.
                    auto *page =
                        dynamic_cast<SearchPage *>(window.findChild<QWidget *>("search-page"));
                    const auto position = page->playback_status().position_ms;
                    if (position && loop_peak && position + 500 < loop_peak) {
                        std::cout << "PASS: playback looped from " << loop_peak << " to "
                                  << position << std::endl;
                        stage = 11;
                    } else if (tick > strip_tick + 100) {
                        throw std::runtime_error("Playback did not loop");
                    }
                    loop_peak = std::max(loop_peak, position);
                } else if (stage == 11) {
                    window.grab().save(QString::fromStdString(
                        (std::filesystem::path(argv[1]).parent_path() / "search-ui.png").string()));
                    tabs->setCurrentIndex(0);
                    window.live_view()->activated(0, true);
                    auto *timeline = dynamic_cast<RecordingTimeline *>(
                        window.findChild<QWidget *>("recording-timeline"));
                    timeline->seek_requested(last);
                    stage = 5;
                } else if (stage == 5 && window.recording_view()->displayed_playback_time() > 0) {
                    auto *v = window.recording_view();
                    auto rect = v->displayed_video_rect();
                    if (rect.isEmpty() || !v->isExposed())
                        return;
                    QPointF a = rect.topLeft() + QPointF(rect.width() * .25, rect.height() * .25);
                    QPointF b = rect.topLeft() + QPointF(rect.width() * .6, rect.height() * .75);
                    QMouseEvent press(QEvent::MouseButtonPress, a, a, Qt::LeftButton,
                                      Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(v, &press);
                    QMouseEvent move(QEvent::MouseMove, b, b, Qt::NoButton, Qt::LeftButton,
                                     Qt::NoModifier);
                    QApplication::sendEvent(v, &move);
                    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                    QApplication::sendEvent(v, &escape);
                    if (service->result().request != request)
                        throw std::runtime_error("Cancelled drag submitted a query");
                    QApplication::sendEvent(v, &press);
                    QApplication::sendEvent(v, &move);
                    QMouseEvent release(QEvent::MouseButtonRelease, b, b, Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(v, &release);
                    region_tick = tick;
                    stage = 6;
                } else if (stage == 6) {
                    // The region crop finishes asynchronously before switching modes.
                    if (!window.findChild<QPushButton *>("search-mode-image")->isChecked()) {
                        if (tick < region_tick + 25)
                            return;
                        throw std::runtime_error("Region search did not select image mode");
                    }
                    if (!region_logged) {
                        std::cout << "Image mode after " << tick - region_tick << " ticks"
                                  << std::endl;
                        region_logged = true;
                    }
                    auto r = service->result();
                    if (tabs->currentIndex() == search_tab && r.request > request &&
                        !r.groups.empty()) {
                        if (errors && errors())
                            throw std::runtime_error("Copy audit violations");
                        std::cout << "PASS: live collection, English/Japanese search, result "
                                     "playback, drag image search; groups="
                                  << r.groups.size() << std::endl;
                        for (const auto &source : controller.status())
                            std::cout << "camera=" << source.source.id
                                      << " display_fps=" << source.display_fps
                                      << " background_ai=" << source.background_ai << std::endl;
                        if (live_drag_checks < 2) {
                            request = r.request;
                            tabs->setCurrentIndex(0);
                            window.findChild<QPushButton *>("playback-live")->click();
                            stage = 8;
                            return;
                        }
                        if (argc == 3) {
                            window.findChild<QPushButton *>("playback-live")->click();
                            auto *page = dynamic_cast<SearchPage *>(
                                window.findChild<QWidget *>("search-page"));
                            auto release = page->release().share();
                            controller.delete_closed_recordings(release);
                            deletion_start = tick;
                            stage = 7;
                        } else
                            app.quit();
                    }
                } else if (stage == 8 && window.live_view()->isExposed()) {
                    auto *v = window.live_view();
                    auto rect = v->displayed_video_rect();
                    if (rect.isEmpty())
                        return;
                    v->hold_live(live_drag_checks == 0);
                    QPointF a = rect.topLeft() + QPointF(rect.width() * .2, rect.height() * .2);
                    QPointF b = rect.topLeft() + QPointF(rect.width() * .7, rect.height() * .8);
                    QMouseEvent press(QEvent::MouseButtonPress, a, a, Qt::LeftButton,
                                      Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent move(QEvent::MouseMove, b, b, Qt::NoButton, Qt::LeftButton,
                                     Qt::NoModifier);
                    QMouseEvent release(QEvent::MouseButtonRelease, b, b, Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                    capture_live("live-before-drag.png");
                    QApplication::sendEvent(v, &press);
                    QApplication::sendEvent(v, &move);
                    drag_end = b;
                    // grabFramebuffer() reads the last painted frame, so capture the
                    // selection on the next tick before releasing.
                    if (qEnvironmentVariableIsSet("AMDVA_VALIDATE_CAPTURE")) {
                        stage = 18;
                        return;
                    }
                    QApplication::sendEvent(v, &release);
                    ++live_drag_checks;
                    stage = 6;
                } else if (stage == 18) {
                    capture_live("live-dragging.png");
                    QMouseEvent release(QEvent::MouseButtonRelease, drag_end, drag_end,
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(window.live_view(), &release);
                    ++live_drag_checks;
                    stage = 6;
                } else if (stage == 7 && tick > deletion_start + 10) {
                    auto state = controller.deletion_status();
                    if (state.state != "running" && state.state != "waiting") {
                        if (!controller.search_index()
                                 .image(observation_id)
                                 .empty())
                            throw std::runtime_error("Deleted recording retained its image");
                        if (errors && errors())
                            throw std::runtime_error("Copy audit violations");
                        std::cout << "PASS: deletion removed search image; deleted="
                                  << state.deleted << std::endl;
                        for (const auto *name :
                             {"amdva_audit_search_bytes", "amdva_audit_embedding_bytes"}) {
                            auto counter = reinterpret_cast<unsigned long long (*)()>(
                                dlsym(RTLD_DEFAULT, name));
                            if (counter)
                                std::cout << name << "=" << counter() << std::endl;
                        }
                        deletion_start = tick;
                        stage = 15;
                    }
                } else if (stage == 15 && tick > deletion_start + 8) {
                    // Deletion prunes the shown results instead of clearing them all.
                    const auto text = window.findChild<QLabel *>("search-status")->text();
                    int cards = 0;
                    for (auto *b : window.findChildren<QAbstractButton *>())
                        cards += b->objectName().startsWith("search-result-");
                    if (text.contains("Recordings changed") ||
                        !(cards ? text.startsWith(QString::number(cards) + " ")
                                : text.contains("deleted")))
                        throw std::runtime_error("Search results after deletion: " +
                                                 text.toStdString() + " cards=" +
                                                 std::to_string(cards));
                    std::cout << "PASS: deletion pruned results; cards=" << cards << " status="
                              << text.toStdString() << std::endl;
                    exiting = true;
                    stage = 99;
                    window.findChild<QPushButton *>("exit-application")->click();
                }
            } catch (const std::exception &e) {
                std::cerr << e.what() << std::endl;
                failed = 1;
                app.quit();
            }
        });
        timer.start(200);
        app.exec();
        if (exiting && !failed)
            std::cout << "PASS: Exit Application quit the event loop" << std::endl;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return failed;
}
