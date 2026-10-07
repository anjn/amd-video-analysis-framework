#include "window.hpp"
#include "amdva/overlay.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <set>
#include <unistd.h>
namespace amdva::nvr {
namespace {
volatile std::sig_atomic_t shutdown_requested = 0;
void shutdown_signal(int) { shutdown_requested = 1; }
QString qs(const std::string &s) { return QString::fromStdString(s); }
GLuint program(QOpenGLExtraFunctions *gl, const char *vertex, const char *fragment) {
    GLuint shaders[2]{};
    GLuint result = 0;
    try {
        for (unsigned i = 0; i < 2; ++i) {
            shaders[i] = gl->glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
            const char *text = i ? fragment : vertex;
            gl->glShaderSource(shaders[i], 1, &text, nullptr);
            gl->glCompileShader(shaders[i]);
            GLint ok = 0;
            gl->glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char message[4096]{};
                gl->glGetShaderInfoLog(shaders[i], sizeof(message), nullptr, message);
                throw std::runtime_error(message);
            }
        }
        result = gl->glCreateProgram();
        for (auto shader : shaders)
            gl->glAttachShader(result, shader);
        gl->glLinkProgram(result);
        GLint ok = 0;
        gl->glGetProgramiv(result, GL_LINK_STATUS, &ok);
        if (!ok) {
            char message[4096]{};
            gl->glGetProgramInfoLog(result, sizeof(message), nullptr, message);
            throw std::runtime_error(message);
        }
    } catch (...) {
        for (auto shader : shaders)
            if (shader)
                gl->glDeleteShader(shader);
        if (result)
            gl->glDeleteProgram(result);
        throw;
    }
    for (auto shader : shaders)
        gl->glDeleteShader(shader);
    return result;
}
} // namespace
Tiles::Tiles(Controller &c) : QOpenGLWindow(QOpenGLWindow::NoPartialUpdate), controller_(c) {
    connect(this, &QOpenGLWindow::frameSwapped, this, [this] {
        const auto now = Telemetry::monotonic_ns();
        for (auto &pending : pending_swap_) {
            if (!pending.timing.mark(TimingStage::Swap, now))
                continue;
            auto &slot = controller_.gpu_slots()[pending.slot];
            std::lock_guard<std::mutex> lock(slot.mutex);
            auto &trace = slot.preview[pending.plane].timing;
            if (trace.telemetry == pending.timing.telemetry &&
                trace.frame.generation == pending.timing.frame.generation &&
                trace.frame.sequence == pending.timing.frame.sequence)
                trace = pending.timing;
        }
        pending_swap_.clear();
    });
    connect(this, &QWindow::visibleChanged, this, [this](bool) { observe_visibility(); });
    connect(this, &QWindow::visibilityChanged, this, [this](QWindow::Visibility value) {
        (void)value;
        observe_visibility();
    });
    current_.fill(-1);
}
void Tiles::observe_visibility() {
    const bool active = isVisible() && isExposed() && visibility() != QWindow::Minimized;
    controller_.presentation_active(active);
    if (!active) {
        for (const auto &pending : pending_swap_)
            if (pending.timing.telemetry)
                pending.timing.telemetry->counter(TimingCounter::PresentationUnobserved);
        pending_swap_.clear();
    }
}
void Tiles::exposeEvent(QExposeEvent *event) {
    QOpenGLWindow::exposeEvent(event);
    observe_visibility();
}
Tiles::~Tiles() {
    if (!context() || !gl_)
        return;
    makeCurrent();
    gl_->glFinish();
    for (auto &group : textures_)
        for (auto &t : group) {
            if (t.fence)
                gl_->glDeleteSync(t.fence);
            if (t.texture)
                gl_->glDeleteTextures(1, &t.texture);
            if (t.image != EGL_NO_IMAGE_KHR && destroy_)
                destroy_(egl_, t.image);
        }
    if (buffer_)
        gl_->glDeleteBuffers(1, &buffer_);
    if (image_program_)
        gl_->glDeleteProgram(image_program_);
    if (box_program_)
        gl_->glDeleteProgram(box_program_);
    doneCurrent();
}
void Tiles::initializeGL() {
    try {
        gl_ = context()->extraFunctions();
        gl_->initializeOpenGLFunctions();
        egl_ = eglGetCurrentDisplay();
        const char *renderer = reinterpret_cast<const char *>(gl_->glGetString(GL_RENDERER));
        if (!context()->isOpenGLES() || egl_ == EGL_NO_DISPLAY || !renderer ||
            std::string(renderer).find("llvmpipe") != std::string::npos ||
            std::string(renderer).find("softpipe") != std::string::npos ||
            (std::string(renderer).find("AMD") == std::string::npos &&
             std::string(renderer).find("Radeon") == std::string::npos))
            throw std::runtime_error("Hardware EGL/OpenGL ES context required");
        create_ =
            reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroy_ =
            reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        target_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!create_ || !destroy_ || !target_)
            throw std::runtime_error("EGL DMA-BUF import unavailable");
        for (unsigned i = 0; i < 16; ++i) {
            auto &s = controller_.gpu_slots()[i];
            std::lock_guard<std::mutex> l(s.mutex);
            for (unsigned j = 0; j < 3; ++j) {
                auto d = s.va->export_surface(s.preview[j].surface);
                try {
                    const auto layout = validate_layout(d);
                    EGLint attrs[] = {EGL_WIDTH,
                                      1920,
                                      EGL_HEIGHT,
                                      1080,
                                      EGL_LINUX_DRM_FOURCC_EXT,
                                      EGLint(d.layers[0].drm_format),
                                      EGL_DMA_BUF_PLANE0_FD_EXT,
                                      d.objects[0].fd,
                                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                      EGLint(layout.y_offset),
                                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                      EGLint(layout.y_pitch),
                                      EGL_NONE};
                    auto &t = textures_[i][j];
                    t.image = create_(egl_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
                    if (t.image == EGL_NO_IMAGE_KHR)
                        throw std::runtime_error("Cannot import VA preview surface into EGL");
                    gl_->glGenTextures(1, &t.texture);
                    gl_->glBindTexture(GL_TEXTURE_2D, t.texture);
                    target_(GL_TEXTURE_2D, t.image);
                    gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    if (gl_->glGetError() != GL_NO_ERROR)
                        throw std::runtime_error("VA texture binding failed");
                } catch (...) {
                    for (unsigned k = 0; k < d.num_objects; ++k)
                        ::close(d.objects[k].fd);
                    throw;
                }
                for (unsigned k = 0; k < d.num_objects; ++k)
                    ::close(d.objects[k].fd);
            }
        }
        image_program_ =
            program(gl_,
                    "#version 300 es\nlayout(location=0) in vec2 p;layout(location=1) in vec2 "
                    "t;out vec2 uv;void main(){gl_Position=vec4(p,0,1);uv=t;}",
                    "#version 300 es\nprecision mediump float;in vec2 uv;uniform sampler2D "
                    "image;out vec4 color;void main(){color=texture(image,uv);}");
        box_program_ = program(
            gl_,
            "#version 300 es\nlayout(location=0) in vec2 p;void main(){gl_Position=vec4(p,0,1);}",
            "#version 300 es\nprecision mediump float;out vec4 color;void "
            "main(){color=vec4(0.1,1.0,0.2,1.0);}");
        gl_->glGenBuffers(1, &buffer_);
    } catch (const std::exception &e) {
        error_ = e.what();
    }
}
void Tiles::vertices(const float *data, unsigned count, unsigned stride) {
    gl_->glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    gl_->glBufferData(GL_ARRAY_BUFFER, count * stride * sizeof(float), data, GL_STREAM_DRAW);
    gl_->glEnableVertexAttribArray(0);
    gl_->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float), nullptr);
    if (stride == 4) {
        gl_->glEnableVertexAttribArray(1);
        gl_->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float),
                                   reinterpret_cast<void *>(2 * sizeof(float)));
    } else
        gl_->glDisableVertexAttribArray(1);
}
void Tiles::release_current(unsigned slot) {
    const int old = current_[slot];
    if (old < 0)
        return;
    auto &s = controller_.gpu_slots()[slot];
    auto &t = textures_[slot][old];
    if (t.fence)
        gl_->glDeleteSync(t.fence);
    t.fence = gl_->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s.preview[old].state = 3;
    current_[slot] = -1;
}
void Tiles::paintGL() {
    controller_.presentation_active(isExposed() && visibility() != QWindow::Minimized);
    for (const auto &pending : pending_swap_)
        if (pending.timing.telemetry)
            pending.timing.telemetry->counter(TimingCounter::PresentationUnobserved);
    pending_swap_.clear();
    gl_->glViewport(0, 0, int(width() * devicePixelRatio()), int(height() * devicePixelRatio()));
    gl_->glClearColor(0.025f, 0.025f, 0.035f, 1);
    gl_->glClear(GL_COLOR_BUFFER_BIT);
    if (!error_.empty() || !image_program_)
        return;
    const auto statuses = controller_.status();
    std::vector<int> active;
    for (const auto &status : statuses)
        if (status.slot >= 0)
            active.push_back(status.slot);
    const unsigned side = active.empty() ? 1 : unsigned(std::ceil(std::sqrt(active.size())));
    for (unsigned i = 0; i < 16; ++i) {
        auto &s = controller_.gpu_slots()[i];
        std::lock_guard<std::mutex> l(s.mutex);
        for (unsigned j = 0; j < 3; ++j) {
            auto &p = s.preview[j];
            auto &t = textures_[i][j];
            if (p.state == 3 && t.fence) {
                const auto result = gl_->glClientWaitSync(t.fence, 0, 0);
                if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {
                    gl_->glDeleteSync(t.fence);
                    t.fence = nullptr;
                    p.state = 0;
                }
            }
        }
        if (s.channel.empty()) {
            release_current(i);
            for (auto &p : s.preview)
                if (p.state == 1)
                    p.state = 0;
            continue;
        }
        if (current_[i] >= 0 && s.preview[current_[i]].generation != s.generation)
            release_current(i);
        int newest = -1;
        for (unsigned j = 0; j < 3; ++j)
            if (s.preview[j].state == 1) {
                if (s.preview[j].generation != s.generation) {
                    if (s.preview[j].timing.telemetry)
                        s.preview[j].timing.telemetry->counter(TimingCounter::PreviewDiscarded);
                    s.preview[j].state = 0;
                    continue;
                }
                if (newest < 0 || s.preview[j].sequence > s.preview[newest].sequence)
                    newest = int(j);
            }
        if (newest >= 0) {
            release_current(i);
            current_[i] = newest;
            s.preview[newest].state = 2;
            presented_++;
            for (unsigned j = 0; j < 3; ++j)
                if (s.preview[j].state == 1) {
                    if (s.preview[j].timing.telemetry)
                        s.preview[j].timing.telemetry->counter(TimingCounter::PreviewDiscarded);
                    s.preview[j].state = 0;
                }
        }
    }
    for (unsigned tile = 0; tile < active.size(); ++tile) {
        const unsigned index = active[tile];
        auto &s = controller_.gpu_slots()[index];
        std::lock_guard<std::mutex> l(s.mutex);
        s.target_width = std::max(2u, unsigned(width() * devicePixelRatio() / side));
        s.target_height = std::max(2u, unsigned(height() * devicePixelRatio() / side));
        const int image = current_[index];
        if (image < 0 || s.channel.empty() || s.preview[image].generation != s.generation)
            continue;
        auto &p = s.preview[image];
        const float cellw = 2.f / side, cellh = 2.f / side;
        const float aspect = float(p.width) / p.height, available = float(width()) / height();
        float w = cellw, h = cellh;
        if (aspect > available)
            h = w * available / aspect;
        else
            w = h * aspect / available;
        const float x = -1 + (tile % side) * cellw + (cellw - w) / 2,
                    y = 1 - (tile / side) * cellh - (cellh - h) / 2;
        const float u = float(p.width) / 1920, v = float(p.height) / 1080;
        const float quad[] = {x, y, 0, 0, x + w, y, u, 0, x, y - h, 0, v, x + w, y - h, u, v};
        gl_->glUseProgram(image_program_);
        gl_->glActiveTexture(GL_TEXTURE0);
        gl_->glBindTexture(GL_TEXTURE_2D, textures_[index][image].texture);
        gl_->glUniform1i(gl_->glGetUniformLocation(image_program_, "image"), 0);
        vertices(quad, 4, 4);
        gl_->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        p.timing.mark(TimingStage::Draw);
        if (p.timing.telemetry && !p.timing.frame.ns[unsigned(TimingStage::Swap)])
            pending_swap_.push_back({index, unsigned(image), p.timing});
        if (Clock::now() - s.detected_at <= std::chrono::milliseconds(500) &&
            p.original_width == s.detection_width && p.original_height == s.detection_height) {
            gl_->glUseProgram(box_program_);
            OverlayFrame overlay{s.detection_width, s.detection_height, {}, {}};
            if (!s.tracked.empty())
                for (const auto &tracked : s.tracked)
                    overlay.objects.push_back({tracked.box, tracked.label, tracked.id, {}});
            else
                for (const auto &box : s.detections)
                    overlay.objects.push_back({box, "", 0, {}});
            const auto lines = make_overlay_lines(overlay, s.detection_width, s.detection_height);
            std::vector<float> points;
            points.reserve(lines.size() * 4);
            for (const auto &line : lines) {
                points.push_back(x + w * line.x0 / s.detection_width);
                points.push_back(y - h * line.y0 / s.detection_height);
                points.push_back(x + w * line.x1 / s.detection_width);
                points.push_back(y - h * line.y1 / s.detection_height);
            }
            if (!points.empty()) {
                vertices(points.data(), points.size() / 2, 2);
                gl_->glDrawArrays(GL_LINES, 0, points.size() / 2);
            }
        }
    }
    gl_->glFlush();
}
void install_shutdown_signals(QCoreApplication &app) {
    shutdown_requested = 0;
    std::signal(SIGINT, shutdown_signal);
    std::signal(SIGTERM, shutdown_signal);
    auto *timer = new QTimer(&app);
    QObject::connect(timer, &QTimer::timeout, &app, [&app] {
        if (shutdown_requested)
            app.quit();
    });
    timer->start(100);
}
Window::Window(Controller &c, Options options, std::string config)
    : controller_(c), options_(std::move(options)), config_(std::move(config)) {
    setWindowTitle("AMD Video Analytics NVR");
    resize(1500, 900);
    auto *widget = new QWidget;
    setCentralWidget(widget);
    auto *layout = new QHBoxLayout(widget);
    auto *sidebar = new QVBoxLayout;
    layout->addLayout(sidebar, 0);
    auto *add = new QPushButton("Add RTSP");
    auto *remove = new QPushButton("Remove selected row");
    sidebar->addWidget(add);
    sidebar->addWidget(remove);
    table_ = new QTableWidget(0, 8);
    table_->setHorizontalHeaderLabels({"View", "Channel", "Connection", "Recording",
                                       "Decode / swap / AI FPS", "Status", "Input→decode P99 ms",
                                       "Input→swap P99 ms"});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    sidebar->addWidget(table_);
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    sidebar->addWidget(summary_);
    tiles_ = new Tiles(c);
    layout->addWidget(QWidget::createWindowContainer(tiles_), 1);
    connect(add, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const auto uri =
            QInputDialog::getText(this, "Add camera", "RTSP URL", QLineEdit::Normal, {}, &ok);
        if (!ok || uri.isEmpty())
            return;
        std::string normalized;
        try {
            normalized = validated_uri(uri.toStdString());
        } catch (const std::exception &e) {
            QMessageBox::warning(this, "Invalid URL", qs(e.what()));
            return;
        }
        const auto name =
            QInputDialog::getText(this, "Camera name", "Name", QLineEdit::Normal, "Camera", &ok);
        if (!ok)
            return;
        Source s{QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                 name.toStdString(), normalized, false};
        controller_.add(s);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto row = table_->currentRow();
        if (row >= 0)
            controller_.remove(table_->item(row, 1)->data(Qt::UserRole).toString().toStdString());
    });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (updating_ || item->column() != 0)
            return;
        controller_.select(
            table_->item(item->row(), 1)->data(Qt::UserRole).toString().toStdString(),
            item->checkState() == Qt::Checked);
    });
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        refresh();
        tiles_->update();
    });
    timer->start(33);
}
Window::~Window() {
    controller_.shutdown();
    try {
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
        check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
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
