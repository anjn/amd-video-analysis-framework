#include "window.hpp"
#include "amdva/overlay.hpp"
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
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
#include <QStyleHints>
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
    auto surface_format = format();
    surface_format.setSamples(4);
    setFormat(surface_format);
    current_.fill(-1);
    click_timer_ = new QTimer(this);
    click_timer_->setSingleShot(true);
    auto *selection_timer = new QTimer(this);
    connect(selection_timer, &QTimer::timeout, this, [this] { poll_selection(); });
    selection_timer->start(20);
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
    if (analysis_query_.valid())
        analysis_query_.wait();
    if (extracting_.valid())
        extracting_.wait();
    if (!context() || !gl_)
        return;
    makeCurrent();
    gl_->glFinish();
    release_preview();
    for (unsigned i = 0; i < slot_count(); ++i) {
        auto &s = slot(i);
        std::lock_guard<std::mutex> lock(s.mutex);
        for (unsigned j = 0; j < 3; ++j)
            if (current_[i] == int(j) || textures_[i][j].fence)
                s.preview[j].state = 0;
    }
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
                                      EGLint(streaming::max_video_width),
                                      EGL_HEIGHT,
                                      EGLint(streaming::max_video_height),
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
            "#version 300 es\nprecision mediump float;uniform vec4 pen;out vec4 color;void "
            "main(){color=pen;}");
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
void Tiles::draw_text_overlay(const QString &text, float x, float y, float w, float h, bool bottom,
                              bool align_right, QColor color, double size) {
    VectorText::Run run;
    try {
        run = overlay_font_.layout(text);
    } catch (const std::exception &e) {
        error_ = e.what();
        return;
    }
    if (run.triangles.empty() || run.width <= 0 || run.height <= 0)
        return;
    const float available_width = width() * w / 2, available_height = height() * h / 2;
    const double scale = std::max(0., std::min({size / 64., (available_width - 16) / run.width,
                                                (available_height - 16) / run.height}));
    if (scale <= 0)
        return;
    const float dx = 2.f / width(), dy = 2.f / height(), pad = 6;
    const float left = align_right ? x + w - (run.width * scale + 8) * dx : x + 8 * dx;
    const float top = bottom ? y - h + (run.height * scale + 8) * dy : y - 8 * dy;
    const float baseline = top - run.ascent * scale * dy;
    std::vector<float> points;
    points.reserve(run.triangles.size() * 2);
    for (const auto &point : run.triangles) {
        points.push_back(left + point.x() * scale * dx);
        points.push_back(baseline - point.y() * scale * dy);
    }
    gl_->glUseProgram(box_program_);
    const auto pen = gl_->glGetUniformLocation(box_program_, "pen");
    // Straight alpha blending leaves the video visible behind the dark panel.
    gl_->glEnable(GL_BLEND);
    gl_->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl_->glUniform4f(pen, 0, 0, 0, .45f);
    const float right = left + (run.width * scale + pad) * dx,
                bottom_edge = top - (run.height * scale + pad) * dy;
    const float background[] = {left - pad * dx, top + pad * dy, right, top + pad * dy,
                                left - pad * dx, bottom_edge,    right, bottom_edge};
    vertices(background, 4, 2);
    gl_->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_->glUniform4f(pen, color.redF(), color.greenF(), color.blueF(), 1);
    vertices(points.data(), points.size() / 2, 2);
    gl_->glDrawArrays(GL_TRIANGLES, 0, points.size() / 2);
    gl_->glDisable(GL_BLEND);
}

void Tiles::draw_detection_overlay(const OverlayFrame &overlay, const QRectF &video) {
    // Geometry uses logical display pixels, so grid size and device scaling never
    // shrink text or depend on the GLES implementation's supported line widths.
    gl_->glUseProgram(box_program_);
    const auto pen = gl_->glGetUniformLocation(box_program_, "pen");
    gl_->glEnable(GL_BLEND);
    gl_->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    auto fill = [&](QRectF r) {
        r = r.intersected(video);
        if (r.isEmpty())
            return;
        float l = 2 * r.left() / width() - 1, t = 1 - 2 * r.top() / height();
        float right = 2 * r.right() / width() - 1, b = 1 - 2 * r.bottom() / height();
        float points[] = {l, t, right, t, l, b, right, b};
        vertices(points, 4, 2);
        gl_->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };
    std::vector<QRectF> labels;
    for (const auto &object : overlay.objects) {
        const auto &b = object.box;
        auto box =
            QRectF(video.left() + video.width() * b.x / overlay.width,
                   video.top() + video.height() * b.y / overlay.height,
                   video.width() * b.w / overlay.width, video.height() * b.h / overlay.height)
                .intersected(video);
        if (box.isEmpty())
            continue;
        const auto tint =
            object.tracking_id
                ? QColor::fromHsvF(std::fmod(object.tracking_id * .618033988749895, 1.), .65, 1.)
                : QColor::fromRgbF(.85, .85, .85);
        const float color[] = {float(tint.redF()), float(tint.greenF()), float(tint.blueF())};
        auto border = [&](float thick) {
            fill({box.left() - thick / 2, box.top() - thick / 2, box.width() + thick, thick});
            fill({box.left() - thick / 2, box.bottom() - thick / 2, box.width() + thick, thick});
            fill({box.left() - thick / 2, box.top(), thick, box.height()});
            fill({box.right() - thick / 2, box.top(), thick, box.height()});
        };
        gl_->glUniform4f(pen, 0, 0, 0, .2f);
        border(5);
        gl_->glUniform4f(pen, color[0], color[1], color[2], .4f);
        border(3);
        if ((!playback_ && cell_count_ > 1 && focus_ < 0) || !object.tracking_id)
            continue;
        QString text;
        if (object.tracking_id)
            text = QString("#%1").arg(object.tracking_id);
        VectorText::Run run;
        try {
            run = overlay_font_.layout(text);
        } catch (const std::exception &e) {
            error_ = e.what();
            continue;
        }
        const double scale = 16. / 64., pad = 4;
        // Keep the font size fixed; remove trailing characters on very small tiles.
        while (run.width * scale + 2 * pad > video.width() && text.size() > 1) {
            text.chop(1);
            run = overlay_font_.layout(text);
        }
        double lw = run.width * scale + 2 * pad, lh = run.height * scale + 2 * pad;
        if (lw > video.width() || lh > video.height())
            continue;
        double left = std::clamp(box.left(), video.left(), video.right() - lw);
        double top = box.top() - lh - 3;
        if (top < video.top())
            top = box.top() + 3;
        top = std::clamp(top, video.top(), video.bottom() - lh);
        QRectF label(left, top, lw, lh);
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            bool collision = std::any_of(labels.begin(), labels.end(),
                                         [&](const auto &r) { return r.intersects(label); });
            if (!collision || label.bottom() + lh + 2 > video.bottom())
                break;
            label.translate(0, lh + 2);
        }
        labels.push_back(label);
        gl_->glUniform4f(pen, 0, 0, 0, .3f);
        fill(label);
        std::vector<float> glyphs;
        for (const auto &point : run.triangles) {
            glyphs.push_back(2 * (label.left() + pad + point.x() * scale) / width() - 1);
            glyphs.push_back(1 -
                             2 * (label.top() + pad + (run.ascent + point.y()) * scale) / height());
        }
        gl_->glUniform4f(pen, color[0], color[1], color[2], .6f);
        vertices(glyphs.data(), glyphs.size() / 2, 2);
        gl_->glDrawArrays(GL_TRIANGLES, 0, glyphs.size() / 2);
    }
    gl_->glDisable(GL_BLEND);
}

void Tiles::draw_pause(const QRectF &rect) {
    const float half = rect.height() * .1f;
    const float cx = rect.center().x(), cy = rect.center().y();
    const float points[] = {2 * (cx - half * .75f) / width() - 1, 1 - 2 * (cy - half) / height(),
                            2 * (cx - half * .75f) / width() - 1, 1 - 2 * (cy + half) / height(),
                            2 * (cx + half * .75f) / width() - 1, 1 - 2 * cy / height()};
    gl_->glUseProgram(box_program_);
    gl_->glEnable(GL_BLEND);
    gl_->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl_->glUniform4f(gl_->glGetUniformLocation(box_program_, "pen"), .7f, .7f, .7f, .55f);
    vertices(points, 3, 2);
    gl_->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl_->glDisable(GL_BLEND);
}

void Tiles::preview_image(bool enabled, const KeyframePreviewStatus &status) {
    if (enabled && !preview_mode_)
        click_timer_->stop();
    preview_mode_ = enabled;
    preview_pending_ = enabled ? status.image : nullptr;
    preview_state_ = status.error.empty() ? status.state : status.error;
    if (!enabled && context() && gl_) {
        makeCurrent();
        release_preview();
        doneCurrent();
    }
    update();
}
void Tiles::release_preview() {
    if (!preview_current_ && !preview_texture_.texture)
        return;
    gl_->glFinish();
    if (preview_texture_.texture)
        gl_->glDeleteTextures(1, &preview_texture_.texture);
    if (preview_texture_.image != EGL_NO_IMAGE_KHR)
        destroy_(egl_, preview_texture_.image);
    preview_texture_ = {};
    preview_current_.reset();
}
void Tiles::draw_recorded(const std::string &channel, const std::string &session, int64_t at,
                          int64_t start, int64_t end, const QRectF &rect) {
    auto &catalog = controller_.recording_catalog();
    const auto revision = catalog.deletion_generation();
    if (analysis_query_.valid() &&
        analysis_query_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            analysis_cache_ = analysis_query_.get();
        } catch (const std::exception &) {
            analysis_cache_ = {};
        }
    }
    const bool valid = analysis_cache_.channel == channel && analysis_cache_.revision == revision &&
                       at >= analysis_cache_.from && at <= analysis_cache_.to;
    if (!analysis_query_.valid() && (!valid || at > analysis_cache_.to - 1000)) {
        analysis_query_ = std::async(std::launch::async, [&catalog, channel, at, revision] {
            AnalysisCache c{channel, at - 2000, at + 5000, revision, {}};
            c.frames = catalog.analysis(channel, c.from, c.to);
            return c;
        });
    }
    if (!valid || at < start || at >= end)
        return;
    const RecordedAnalysis *chosen = nullptr;
    for (const auto &f : analysis_cache_.frames)
        if (f.session == session && f.time_ms >= start && f.time_ms <= at)
            chosen = &f;
    if (!chosen || at - chosen->time_ms >= chosen->valid_ms)
        return;
    OverlayFrame overlay{1, 1, {}, {}};
    for (const auto &o : chosen->objects)
        overlay.objects.push_back({o.box, "", o.person, {}});
    displayed_analysis_objects_ = overlay.objects.size();
    draw_detection_overlay(overlay, rect);
}
void Tiles::draw_preview() {
    try {
        if (preview_pending_ && preview_current_ != preview_pending_) {
            release_preview();
            preview_current_ = preview_pending_;
            if (preview_current_) {
                auto d = preview_current_->export_surface();
                try {
                    auto l = validate_layout(d);
                    EGLint attrs[] = {EGL_WIDTH,
                                      EGLint(preview_current_->width),
                                      EGL_HEIGHT,
                                      EGLint(preview_current_->height),
                                      EGL_LINUX_DRM_FOURCC_EXT,
                                      EGLint(d.layers[0].drm_format),
                                      EGL_DMA_BUF_PLANE0_FD_EXT,
                                      d.objects[0].fd,
                                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                      EGLint(l.y_offset),
                                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                      EGLint(l.y_pitch),
                                      EGL_NONE};
                    preview_texture_.image =
                        create_(egl_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
                    if (preview_texture_.image == EGL_NO_IMAGE_KHR)
                        throw std::runtime_error("Preview DMA-BUF import failed");
                    gl_->glGenTextures(1, &preview_texture_.texture);
                    gl_->glBindTexture(GL_TEXTURE_2D, preview_texture_.texture);
                    target_(GL_TEXTURE_2D, preview_texture_.image);
                    gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    gl_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                } catch (...) {
                    for (unsigned i = 0; i < d.num_objects; ++i)
                        ::close(d.objects[i].fd);
                    throw;
                }
                for (unsigned i = 0; i < d.num_objects; ++i)
                    ::close(d.objects[i].fd);
            }
        }
        if (!preview_current_) {
            draw_text_overlay(qs("Preview · " + preview_state_), -1, 1, 2, 2);
            return;
        }
        const auto &p = *preview_current_;
        float w = 2, h = 2, aspect = float(p.width) / p.height,
              available = float(width()) / height();
        if (aspect > available)
            h = w * available / aspect;
        else
            w = h * aspect / available;
        float x = -w / 2, y = h / 2;
        const float quad[] = {x, y, 0, 0, x + w, y, 1, 0, x, y - h, 0, 1, x + w, y - h, 1, 1};
        gl_->glUseProgram(image_program_);
        gl_->glActiveTexture(GL_TEXTURE0);
        gl_->glBindTexture(GL_TEXTURE_2D, preview_texture_.texture);
        gl_->glUniform1i(gl_->glGetUniformLocation(image_program_, "image"), 0);
        vertices(quad, 4, 4);
        gl_->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        QRectF rect((x + 1) * width() / 2, (1 - y) * height() / 2, w * width() / 2,
                    h * height() / 2);
        draw_recorded(p.channel, p.session, p.frame_ms, p.recording_start_ms, p.recording_end_ms,
                      rect);
        draw_text_overlay(qs(camera_name), x, y, w, h);
        draw_text_overlay(
            "Preview · " +
                QDateTime::fromMSecsSinceEpoch(p.frame_ms).toString("yyyy-MM-dd HH:mm:ss"),
            x, y, w, h, true, true);
        displayed_playback_time_ = p.frame_ms;
    } catch (const std::exception &e) {
        preview_state_ = e.what();
        release_preview();
    }
    gl_->glFlush();
}

void Tiles::paintGL() {
    displayed_analysis_objects_ = 0;
    displayed_playback_time_ = 0;
    displayed_stream_info_.fill({});
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
    if (preview_mode_ && (preview_pending_ || preview_current_)) {
        draw_preview();
        return;
    }
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
        if ((frozen_ || held_ || preview_mode_) && current_[i] >= 0)
            continue;
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
        if (image < 0 || ((!frozen_ && !held_) &&
                          (s.channel.empty() || s.preview[image].generation != s.generation)))
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
        if (playback_ || active.size() == 1)
            video_rect_ = QRectF((x + 1) * width() / 2, (1 - y) * height() / 2, w * width() / 2,
                                 h * height() / 2);
        const float u = float(p.width) / streaming::max_video_width,
                    v = float(p.height) / streaming::max_video_height;
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
        if (!playback_) {
            OverlayFrame overlay{s.detection_width, s.detection_height, {}, {}};
            bool person_tracking = false;
            if (auto *search = controller_.search_service()) {
                const auto tracking = search->tracks(s.channel);
                person_tracking = tracking.generation == p.generation &&
                                  tracking.width == p.original_width &&
                                  tracking.height == p.original_height && tracking.generation != 0;
                if (person_tracking &&
                    Clock::now() - tracking.captured <= std::chrono::seconds(1)) {
                    overlay.width = tracking.width;
                    overlay.height = tracking.height;
                    for (const auto &object : tracking.objects)
                        overlay.objects.push_back({object.box, "Person", object.person, {}});
                }
            }
            if (!person_tracking &&
                Clock::now() - s.detected_at <= std::chrono::milliseconds(500) &&
                p.original_width == s.detection_width && p.original_height == s.detection_height) {
                if (!s.tracked.empty())
                    for (const auto &tracked : s.tracked)
                        overlay.objects.push_back({tracked.box, tracked.label, tracked.id, {}});
                else
                    for (const auto &box : s.detections)
                        overlay.objects.push_back(
                            {box, box.class_id == 0 ? "Person" : "Object", 0, {}});
            }
            if (frozen_ || held_ || preview_mode_)
                overlay = held_overlay_[index];
            else
                held_overlay_[index] = overlay;
            if (overlay.width && overlay.height)
                draw_detection_overlay(overlay,
                                       QRectF((x + 1) * width() / 2, (1 - y) * height() / 2,
                                              w * width() / 2, h * height() / 2));
        }
        if (playback_)
            draw_recorded(p.recording_channel, p.recording_session, p.wall_time_ms,
                          p.recording_start_ms, p.recording_end_ms, video_rect_);
        if (dragging_) {
            auto rect =
                QRectF(selection_start_, selection_end_).normalized().intersected(video_rect_);
            float a = 2 * rect.left() / width() - 1, b = 1 - 2 * rect.top() / height(),
                  c = 2 * rect.right() / width() - 1, d = 1 - 2 * rect.bottom() / height();
            float points[] = {a, b, c, b, c, d, a, d};
            gl_->glUseProgram(box_program_);
            gl_->glUniform4f(gl_->glGetUniformLocation(box_program_, "pen"), 1, .8f, 0, 1);
            vertices(points, 4, 2);
            gl_->glDrawArrays(GL_LINE_LOOP, 0, 4);
        }
        if (playback_ && p.wall_time_ms) {
            draw_text_overlay(
                QDateTime::fromMSecsSinceEpoch(p.wall_time_ms).toString("yyyy-MM-dd HH:mm:ss"), x,
                y, w, h, true, true);
            displayed_playback_time_ = p.wall_time_ms;
        }
        if (playback_ || active.size() == 1) {
            std::string name = camera_name;
            if (!playback_)
                for (const auto &status : statuses)
                    if (status.source.id == s.channel)
                        name = status.source.name;
            draw_text_overlay(qs(name), x, y, w, h);
            if (!playback_)
                draw_text_overlay(held_ ? "Live · Paused" : "Live", x, y, w, h, false, true,
                                  held_ ? QColor(255, 220, 70) : QColor(90, 255, 125));
            if ((held_ || paused_) && !preview_mode_)
                draw_pause(video_rect_);
        }
        if (!playback_ && stream_info_visible_) {
            const auto source =
                std::find_if(statuses.begin(), statuses.end(),
                             [&](const auto &status) { return status.source.id == s.channel; });
            if (source != statuses.end() && source->connection == "live") {
                const auto codec =
                    source->input_codec.empty() ? QString("--") : qs(source->input_codec);
                const auto resolution =
                    source->input_width && source->input_height
                        ? QString("%1x%2").arg(source->input_width).arg(source->input_height)
                        : QString("--x--");
                auto text = QString("%1  %2  %3 fps  %4 Mbps")
                                .arg(codec, resolution)
                                .arg(source->input_fps, 0, 'f', 1)
                                .arg(source->input_mbps, 0, 'f', 1);
                const auto latency = decode_latency.find(s.channel);
                text += latency == decode_latency.end()
                            ? "  decode N/A"
                            : QString("  decode %1 ms").arg(latency->second, 0, 'f', 1);
                draw_text_overlay(text, x, y, w, h, true, false, Qt::white,
                                  active.size() == 1 ? 22 : 14);
                displayed_stream_info_[focus_ >= 0 ? unsigned(focus_) : tile] = text;
            }
        }
    }
    gl_->glFlush();
    if (cell_count_ > 1 && focus_ < 0 && !playback_) {
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
    if (cells_ != cells || cell_count_ != count || focus_ != focus) {
        cancel_selection();
        held_ = false;
    }
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
void Tiles::cancel_selection() {
    click_timer_->stop();
    const bool was_selecting = frozen_;
    selection_cancelled_ = true;
    dragging_ = false;
    if (!extracting_.valid())
        frozen_ = false;
    if (was_selecting && selection_cancelled)
        selection_cancelled();
    update();
}
void Tiles::poll_selection() {
    if (!extracting_.valid() ||
        extracting_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    frozen_ = false;
    dragging_ = false;
    try {
        auto image = extracting_.get();
        if (!selection_cancelled_ && region_selected)
            region_selected(std::move(image));
    } catch (const std::exception &e) {
        if (selection_error)
            selection_error(e.what());
    }
    update();
}
void Tiles::mousePressEvent(QMouseEvent *e) {
    if (preview_mode_ || e->button() != Qt::LeftButton)
        return;
    selection_cancelled_ = false;
    selection_slot_ = 0;
    if (!playback_) {
        for (const auto &st : controller_.status())
            if (st.source.id == cells_[picked(e->position())] && st.slot >= 0)
                selection_slot_ = unsigned(st.slot);
    }
    if ((playback_ || focus_ >= 0 || cell_count_ == 1) && region_selected &&
        current_[selection_slot_] >= 0 && video_rect_.contains(e->position())) {
        if (extracting_.valid())
            return;
        frozen_ = true;
        dragging_ = false;
        selection_cancelled_ = false;
        selection_start_ = selection_end_ = e->position();
    }
}
void Tiles::mouseMoveEvent(QMouseEvent *e) {
    if (!frozen_ || extracting_.valid())
        return;
    selection_end_ = e->position();
    if (!dragging_ && (selection_end_ - selection_start_).manhattanLength() >=
                          QGuiApplication::styleHints()->startDragDistance()) {
        dragging_ = true;
        if (selection_started)
            selection_started();
    }
    update();
}
void Tiles::mouseReleaseEvent(QMouseEvent *e) {
    if (preview_mode_ || extracting_.valid() || e->button() != Qt::LeftButton)
        return;
    if (selection_cancelled_) {
        selection_cancelled_ = false;
        return;
    }
    if (!dragging_) {
        frozen_ = false;
        const auto index = picked(e->position());
        click_timer_->disconnect();
        connect(click_timer_, &QTimer::timeout, this, [this, index] {
            if (activated)
                activated(index, false);
        });
        click_timer_->start(QGuiApplication::styleHints()->mouseDoubleClickInterval());
        return;
    }
    selection_end_ = e->position();
    auto rect = QRectF(selection_start_, selection_end_).normalized().intersected(video_rect_);
    if (rect.width() < 2 || rect.height() < 2) {
        cancel_selection();
        return;
    }
    auto &s = slot(selection_slot_);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (current_[selection_slot_] < 0) {
        cancel_selection();
        return;
    }
    auto &p = s.preview[current_[selection_slot_]];
    Detection box;
    box.x = (rect.x() - video_rect_.x()) / video_rect_.width() * p.width;
    box.y = (rect.y() - video_rect_.y()) / video_rect_.height() * p.height;
    box.w = rect.width() / video_rect_.width() * p.width;
    box.h = rect.height() / video_rect_.height() * p.height;
    auto surface = p.surface;
    auto width = p.width, height = p.height;
    extracting_ = std::async(std::launch::async, [this, surface, width, height, box] {
        streaming::SearchCropper cropper(controller_.display(), search_image_size, search_device);
        return cropper.crop(surface, width, height, box);
    });
}
void Tiles::mouseDoubleClickEvent(QMouseEvent *e) {
    click_timer_->stop();
    cancel_selection();
    if (e->button() != Qt::LeftButton)
        return;
    if (activated)
        activated(picked(e->position()), true);
    selection_cancelled_ = true;
}
void Tiles::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && frozen_) {
        cancel_selection();
        return;
    }
    if (e->key() == Qt::Key_Escape && (focus_ >= 0 || playback_ || cell_count_ == 1) && activated)
        activated(unsigned(focus_), true);
    else
        QOpenGLWindow::keyPressEvent(e);
}
} // namespace amdva::nvr
