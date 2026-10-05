#include "window.hpp"
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
std::string validated_uri(std::string uri) {
    const QUrl url(qs(uri));
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != "rtsp" && url.scheme() != "rtsps"))
        throw std::runtime_error("Source URI must use RTSP with a host");
    uri.replace(0, size_t(url.scheme().size()), url.scheme().toStdString());
    return uri;
}
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
    current_.fill(-1);
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
                if (s.preview[j].state == 1)
                    s.preview[j].state = 0;
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
        if (Clock::now() - s.detected_at <= std::chrono::milliseconds(500) &&
            p.original_width == s.detection_width && p.original_height == s.detection_height) {
            gl_->glUseProgram(box_program_);
            for (const auto &b : s.detections) {
                const float left = x + w * b.x / s.detection_width,
                            right = left + w * b.w / s.detection_width,
                            top = y - h * b.y / s.detection_height,
                            bottom = top - h * b.h / s.detection_height;
                const float outline[] = {left, top, right, top, right, bottom, left, bottom};
                vertices(outline, 4, 2);
                gl_->glDrawArrays(GL_LINE_LOOP, 0, 4);
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
Options load_config(const std::string &path) {
    QFile f(qs(path));
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read NVR config");
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("Invalid NVR JSON config");
    const auto j = doc.object();
    Options o;
    const auto resolve = [&](const char *name) {
        const auto text = j[name].toString();
        return text.isEmpty()
                   ? std::string{}
                   : QDir(QFileInfo(f).absolutePath()).absoluteFilePath(text).toStdString();
    };
    o.model = resolve("model");
    o.single_model = resolve("single_model");
    o.model_info = resolve("model_info");
    if (j.contains("recording_root"))
        o.recording_root = resolve("recording_root");
    else
        o.recording_root =
            QDir(QFileInfo(f).absolutePath()).absoluteFilePath("recordings").toStdString();
    const auto number = [&](const char *key, double fallback) {
        if (!j.contains(key))
            return fallback;
        if (!j[key].isDouble())
            throw std::runtime_error(std::string("Expected numeric ") + key);
        return j[key].toDouble();
    };
    const double batch = number("batch_size", 4), seconds = number("segment_seconds", 60);
    if (batch != 1 && batch != 2 && batch != 4 && batch != 8 && batch != 16)
        throw std::runtime_error("Invalid batch size");
    if (seconds != std::floor(seconds) || seconds < 1 || seconds > 86400)
        throw std::runtime_error("Invalid segment duration");
    if (j.contains("background_ai_enabled") && !j["background_ai_enabled"].isBool())
        throw std::runtime_error("Expected boolean background_ai_enabled");
    o.background_ai_enabled = j["background_ai_enabled"].toBool(false);
    const double interval = number("background_ai_interval_seconds", 5);
    if (interval != std::floor(interval) || interval < 1 || interval > 3600)
        throw std::runtime_error("Invalid background AI interval");
    o.background_ai_interval_seconds = unsigned(interval);
    o.batch = unsigned(batch);
    const int segment = int(seconds);
    const double quota = number("quota_gib", 100), reserve = number("reserve_gib", 1);
    if (segment < 1 || segment > 86400 || !std::isfinite(quota) || quota <= 0 || quota > 1000000 ||
        !std::isfinite(reserve) || reserve < 0 || reserve > 1000000)
        throw std::runtime_error("Invalid segment or storage limits");
    o.segment_seconds = unsigned(segment);
    o.quota = uint64_t(quota * 1024 * 1024 * 1024);
    o.reserve = uint64_t(reserve * 1024 * 1024 * 1024);
    std::set<std::string> ids;
    if (o.model.empty() || o.model_info.empty() || (o.batch > 1 && o.single_model.empty()))
        throw std::runtime_error("model, model_info and single_model are required");
    if (j.contains("sources") && !j["sources"].isArray())
        throw std::runtime_error("sources must be an array");
    for (const auto v : j["sources"].toArray()) {
        const auto s = v.toObject();
        Source source{
            s["id"].toString(QUuid::createUuid().toString(QUuid::WithoutBraces)).toStdString(),
            s["name"].toString().toStdString(), s["uri"].toString().toStdString(),
            s["selected"].toBool()};
        source.uri = validated_uri(source.uri);
        if (source.id.empty())
            source.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        if (source.id.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                std::string::npos ||
            !ids.insert(source.id).second)
            throw std::runtime_error("Invalid or duplicate channel ID");
        if (source.name.empty())
            source.name = source.id;
        o.sources.push_back(std::move(source));
    }
    return o;
}
void save_config(const std::string &path, const Options &o) {
    QJsonObject j{{"model", qs(o.model)},
                  {"single_model", qs(o.single_model)},
                  {"model_info", qs(o.model_info)},
                  {"recording_root", qs(o.recording_root)},
                  {"batch_size", int(o.batch)},
                  {"background_ai_enabled", o.background_ai_enabled},
                  {"background_ai_interval_seconds", int(o.background_ai_interval_seconds)},
                  {"segment_seconds", int(o.segment_seconds)},
                  {"quota_gib", double(o.quota) / (1024 * 1024 * 1024)},
                  {"reserve_gib", double(o.reserve) / (1024 * 1024 * 1024)}};
    QJsonArray sources;
    for (const auto &s : o.sources)
        sources.append(QJsonObject{
            {"id", qs(s.id)}, {"name", qs(s.name)}, {"uri", qs(s.uri)}, {"selected", s.selected}});
    j["sources"] = sources;
    QSaveFile f(qs(path));
    if (!f.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot save NVR config");
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (f.write(QJsonDocument(j).toJson()) < 0 || !f.commit())
        throw std::runtime_error("Cannot commit NVR config");
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
    table_ = new QTableWidget(0, 6);
    table_->setHorizontalHeaderLabels(
        {"View", "Channel", "Connection", "Recording", "Display / AI FPS", "Status"});
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
    updating_ = true;
    table_->setRowCount(int(statuses.size()));
    for (unsigned row = 0; row < statuses.size(); ++row) {
        const auto &s = statuses[row];
        for (unsigned col = 0; col < 6; ++col)
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
    summary_->setText(QString("%1 sources · batch %2 · %3 ms\n%4\n%5")
                          .arg(statuses.size())
                          .arg(controller_.last_batch())
                          .arg(controller_.batch_ms(), 0, 'f', 1)
                          .arg(qs(controller_.fatal()))
                          .arg(qs(tiles_->error())));
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
