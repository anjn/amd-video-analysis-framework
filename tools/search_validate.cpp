#include "search_page.hpp"
#include "window.hpp"
#include <QApplication>
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
        auto *input = window.findChild<QLineEdit *>("search-text");
        auto *submit = window.findChild<QPushButton *>("search-submit");
        auto audit = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        bool auditing = false;
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        QTimer timer;
        int tick = 0, stage = 0;
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
                    tabs->setCurrentIndex(3);
                    input->setText("a person wearing a purple skirt");
                    submit->click();
                    stage = 1;
                } else if (stage == 1) {
                    auto r = service->result();
                    if (!r.error.empty())
                        throw std::runtime_error(r.error);
                    if (r.request && !r.groups.empty()) {
                        std::cout << "English groups=" << r.groups.size()
                                  << " best=" << r.groups.front().similarity << std::endl;
                        observation_id = r.groups.front().images.front().id;
                        auto png = controller.recording_catalog().observation_image(observation_id);
                        QFile picture(QString::fromStdString(
                            (std::filesystem::path(argv[1]).parent_path() / "english-match.png")
                                .string()));
                        if (picture.open(QIODevice::WriteOnly))
                            picture.write(reinterpret_cast<const char *>(png.data()), png.size());
                        request = r.request;
                        input->setText(QString::fromUtf8("紫色のスカートを着た人"));
                        submit->click();
                        stage = 2;
                    }
                } else if (stage == 2) {
                    auto r = service->result();
                    if (r.request > request && !r.groups.empty()) {
                        std::cout << "Japanese groups=" << r.groups.size()
                                  << " best=" << r.groups.front().similarity << std::endl;
                        request = r.request;
                        last = r.groups.front().from_ms;
                        stage = 3;
                    }
                } else if (stage == 3) {
                    auto *card = window.findChild<QWidget *>("search-result-0");
                    if (card) {
                        auto buttons = card->findChildren<QPushButton *>();
                        if (!buttons.empty()) {
                            buttons.front()->click();
                            stage = 4;
                        }
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
                        tabs->setCurrentIndex(1);
                        stage = 5;
                    }
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
                    stage = 6;
                } else if (stage == 6) {
                    auto r = service->result();
                    if (tabs->currentIndex() == 3 && r.request > request && !r.groups.empty()) {
                        if (errors && errors())
                            throw std::runtime_error("Copy audit violations");
                        std::cout << "PASS: live collection, English/Japanese search, result "
                                     "playback, drag image search; groups="
                                  << r.groups.size() << std::endl;
                        for (const auto &source : controller.status())
                            std::cout << "camera=" << source.source.id
                                      << " display_fps=" << source.display_fps
                                      << " background_ai=" << source.background_ai << std::endl;
                        if (argc == 3) {
                            window.findChild<QPushButton *>("playback-stop")->click();
                            auto *page = dynamic_cast<SearchPage *>(
                                window.findChild<QWidget *>("search-page"));
                            auto release = page->release().share();
                            controller.delete_closed_recordings(release);
                            deletion_start = tick;
                            stage = 7;
                        } else
                            app.quit();
                    }
                } else if (stage == 7 && tick > deletion_start + 10) {
                    auto state = controller.deletion_status();
                    if (state.state != "running" && state.state != "waiting") {
                        if (!controller.recording_catalog()
                                 .observation_image(observation_id)
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
                        app.quit();
                    }
                }
            } catch (const std::exception &e) {
                std::cerr << e.what() << std::endl;
                failed = 1;
                app.quit();
            }
        });
        timer.start(200);
        app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return failed;
}
