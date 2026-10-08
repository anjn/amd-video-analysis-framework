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
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
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
Tiles::Tiles(Controller &c, Slot *playback)
    : QOpenGLWindow(QOpenGLWindow::NoPartialUpdate), controller_(c), playback_(playback) {
    connect(this, &QOpenGLWindow::frameSwapped, this, [this] {
        const auto now = Telemetry::monotonic_ns();
        for (auto &pending : pending_swap_) {
            if (!pending.timing.mark(TimingStage::Swap, now))
                continue;
            auto &slot = this->slot(pending.slot);
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
    if (!playback_)
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
        for (unsigned i = 0; i < slot_count(); ++i) {
            auto &s = slot(i);
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
    auto &s = this->slot(slot);
    auto &t = textures_[slot][old];
    if (t.fence)
        gl_->glDeleteSync(t.fence);
    t.fence = gl_->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s.preview[old].state = 3;
    current_[slot] = -1;
}
void Tiles::paintGL() {
    if (!playback_)
        controller_.presentation_active(isExposed() && visibility() != QWindow::Minimized);
    for (const auto &pending : pending_swap_)
        if (pending.timing.telemetry)
            pending.timing.telemetry->counter(TimingCounter::PresentationUnobserved);
    pending_swap_.clear();
    gl_->glDisable(GL_SCISSOR_TEST);
    gl_->glDisable(GL_BLEND);
    gl_->glDisable(GL_DEPTH_TEST);
    gl_->glViewport(0, 0, int(width() * devicePixelRatio()), int(height() * devicePixelRatio()));
    gl_->glClearColor(0.025f, 0.025f, 0.035f, 1);
    gl_->glClear(GL_COLOR_BUFFER_BIT);
    if (!error_.empty() || !image_program_)
        return;
    const auto statuses = controller_.status();
    std::vector<int> active;
    if (playback_)
        active.push_back(0);
    else if (cell_count_) {
        for (unsigned tile = 0; tile < cell_count_; ++tile) {
            if (focus_ >= 0 && int(tile) != focus_)
                continue;
            int index = -1;
            for (const auto &s : statuses)
                if (s.source.id == cells_[tile])
                    index = s.slot;
            active.push_back(index);
        }
    } else
        for (const auto &s : statuses)
            if (s.slot >= 0)
                active.push_back(s.slot);
    const unsigned side = active.empty() ? 1 : unsigned(std::ceil(std::sqrt(active.size())));
    for (unsigned i = 0; i < slot_count(); ++i) {
        auto &s = slot(i);
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
        if (active[tile] < 0)
            continue;
        const unsigned index = active[tile];
        auto &s = slot(index);
        std::lock_guard<std::mutex> l(s.mutex);
        s.target_width = std::max(2u, unsigned(width() * devicePixelRatio() / side));
        s.target_height = std::max(2u, unsigned(height() * devicePixelRatio() / side));
        if (!playback_ && cell_count_ && s.channel != cells_[focus_ >= 0 ? unsigned(focus_) : tile])
            continue;
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
    if (cell_count_ && !playback_) {
        QPainter painter(this);
        const unsigned shown = focus_ >= 0 ? 1 : cell_count_;
        const unsigned side = unsigned(std::ceil(std::sqrt(shown)));
        for (unsigned tile = 0; tile < shown; tile++) {
            const unsigned cell = focus_ >= 0 ? unsigned(focus_) : tile;
            QRect rect(int(tile % side) * width() / side, int(tile / side) * height() / side,
                       width() / side, height() / side);
            QString label = QString("%1 · Empty tile").arg(cell + 1);
            for (const auto &s : statuses)
                if (s.source.id == cells_[cell])
                    label = QString("%1 · %2 · %3")
                                .arg(cell + 1)
                                .arg(qs(s.source.name), qs(s.connection));
            painter.fillRect(QRect(rect.left(), rect.top(), rect.width(), 26),
                             QColor(15, 20, 28, 215));
            painter.setPen(Qt::white);
            painter.drawText(rect.adjusted(8, 2, -8, -2), Qt::AlignLeft | Qt::AlignTop, label);
            painter.setPen(QPen(cell == selected_ ? QColor(75, 170, 255) : QColor(75, 80, 90),
                                cell == selected_ ? 3 : 1));
            painter.drawRect(rect.adjusted(1, 1, -2, -2));
        }
    }
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

void Tiles::layout(const std::array<std::string, 16> &cells, unsigned count, int focus) {
    cells_ = cells;
    cell_count_ = count;
    focus_ = focus;
    if (!playback_) {
        std::vector<std::string> visible;
        for (unsigned i = 0; i < count; i++)
            if ((focus < 0 || int(i) == focus) && !cells[i].empty())
                visible.push_back(cells[i]);
        controller_.preview_sources(visible);
    }
    update();
}
unsigned Tiles::picked(const QPointF &p) const {
    if (focus_ >= 0)
        return unsigned(focus_);
    const auto side = std::max(1u, unsigned(std::ceil(std::sqrt(cell_count_ ? cell_count_ : 1))));
    return std::min(side * side - 1,
                    unsigned(std::max(0., p.x()) * side / std::max(1, width())) +
                        side * unsigned(std::max(0., p.y()) * side / std::max(1, height())));
}
void Tiles::mousePressEvent(QMouseEvent *e) {
    if (activated)
        activated(picked(e->position()), false);
}
void Tiles::mouseDoubleClickEvent(QMouseEvent *e) {
    if (activated)
        activated(picked(e->position()), true);
}
void Tiles::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && focus_ >= 0 && activated)
        activated(unsigned(focus_), true);
    else
        QOpenGLWindow::keyPressEvent(e);
}
} // namespace amdva::nvr
