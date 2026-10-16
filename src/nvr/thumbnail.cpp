#include "thumbnail.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWindow>
#include <QWidget>
#include <algorithm>
#include <unistd.h>
namespace amdva::nvr {
using amdva::streaming::GpuPreviewImage;
class ThumbnailWindow final : public QOpenGLWindow {
  public:
    std::shared_ptr<const GpuPreviewImage> pending, current;
    std::string error;
    uint64_t request{}, painted_request{};
    ThumbnailWindow() : QOpenGLWindow(QOpenGLWindow::NoPartialUpdate) {}
    ~ThumbnailWindow() override {
        if (!context() || !gl)
            return;
        makeCurrent();
        release();
        program.reset();
        if (vbo)
            gl->glDeleteBuffers(1, &vbo);
        doneCurrent();
    }
    void image(std::shared_ptr<const GpuPreviewImage> next) {
        pending = std::move(next);
        if (!pending && context() && gl) {
            makeCurrent();
            release();
            doneCurrent();
        }
        update();
    }

  protected:
    void initializeGL() override {
        try {
            gl = context()->extraFunctions();
            gl->initializeOpenGLFunctions();
            egl = eglGetCurrentDisplay();
            auto *renderer = reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER));
            if (!context()->isOpenGLES() || !renderer ||
                (std::string(renderer).find("AMD") == std::string::npos &&
                 std::string(renderer).find("Radeon") == std::string::npos))
                throw std::runtime_error("Hardware EGL preview required");
            create =
                reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
            destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
                eglGetProcAddress("eglDestroyImageKHR"));
            target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
                eglGetProcAddress("glEGLImageTargetTexture2DOES"));
            if (!create || !destroy || !target)
                throw std::runtime_error("EGL import unavailable");
            program = std::make_unique<QOpenGLShaderProgram>();
            if (!program->addShaderFromSourceCode(
                    QOpenGLShader::Vertex,
                    "#version 300 es\nlayout(location=0) in vec2 p;layout(location=1) in vec2 "
                    "t;out vec2 uv;void main(){gl_Position=vec4(p,0,1);uv=t;}") ||
                !program->addShaderFromSourceCode(
                    QOpenGLShader::Fragment,
                    "#version 300 es\nprecision mediump float;in vec2 uv;uniform sampler2D "
                    "image;out vec4 color;void main(){color=texture(image,uv);}") ||
                !program->link())
                throw std::runtime_error("Preview shader compilation failed");
            gl->glGenBuffers(1, &vbo);
        } catch (const std::exception &e) {
            error = e.what();
        }
    }
    void paintGL() override {
        painted_request = 0;
        if (!gl || !isExposed())
            return;
        // Qt may schedule a final paint while the native popup is being hidden.
        // Only draw into a complete window framebuffer.
        gl->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return;
        gl->glViewport(0, 0, int(width() * devicePixelRatio()), int(height() * devicePixelRatio()));
        gl->glClearColor(0.02f, 0.02f, 0.03f, 1);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        if (!error.empty())
            return;
        try {
            if (current != pending) {
                release();
                current = pending;
                if (current) {
                    auto d = current->export_surface();
                    try {
                        auto layout = amdva::validate_layout(d);
                        EGLint attrs[] = {EGL_WIDTH,
                                          EGLint(current->width),
                                          EGL_HEIGHT,
                                          EGLint(current->height),
                                          EGL_LINUX_DRM_FOURCC_EXT,
                                          EGLint(d.layers[0].drm_format),
                                          EGL_DMA_BUF_PLANE0_FD_EXT,
                                          d.objects[0].fd,
                                          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                          EGLint(layout.y_offset),
                                          EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                          EGLint(layout.y_pitch),
                                          EGL_NONE};
                        imported =
                            create(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
                        if (imported == EGL_NO_IMAGE_KHR)
                            throw std::runtime_error("Preview DMA-BUF import failed");
                        gl->glGenTextures(1, &texture);
                        gl->glBindTexture(GL_TEXTURE_2D, texture);
                        target(GL_TEXTURE_2D, imported);
                        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    } catch (...) {
                        for (unsigned i = 0; i < d.num_objects; i++)
                            ::close(d.objects[i].fd);
                        throw;
                    }
                    for (unsigned i = 0; i < d.num_objects; i++)
                        ::close(d.objects[i].fd);
                }
            }
            if (current) {
                float x = 1, y = 1;
                const float aspect = float(current->width) / current->height,
                            available = float(width()) / std::max(1, height());
                if (aspect > available)
                    y = available / aspect;
                else
                    x = aspect / available;
                const float vertices[] = {-x, y, 0, 0, x, y, 1, 0, -x, -y, 0, 1, x, -y, 1, 1};
                program->bind();
                program->setUniformValue("image", 0);
                gl->glActiveTexture(GL_TEXTURE0);
                gl->glBindTexture(GL_TEXTURE_2D, texture);
                gl->glBindBuffer(GL_ARRAY_BUFFER, vbo);
                gl->glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
                for (unsigned i = 0; i < 2; i++) {
                    gl->glEnableVertexAttribArray(i);
                    gl->glVertexAttribPointer(i, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                              reinterpret_cast<void *>(i * 2 * sizeof(float)));
                }
                gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            gl->glFlush();
            if (auto code = gl->glGetError(); code != GL_NO_ERROR)
                throw std::runtime_error("Preview GL rendering failed: " + std::to_string(code));
            painted_request = current ? request : 0;
        } catch (const std::exception &e) {
            error = e.what();
            release();
        }
    }

  private:
    QOpenGLExtraFunctions *gl{};
    std::unique_ptr<QOpenGLShaderProgram> program;
    GLuint texture{}, vbo{};
    EGLDisplay egl{EGL_NO_DISPLAY};
    EGLImageKHR imported{EGL_NO_IMAGE_KHR};
    PFNEGLCREATEIMAGEKHRPROC create{};
    PFNEGLDESTROYIMAGEKHRPROC destroy{};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target{};
    void release() {
        // Small preview updates only. Wait for old GPU sampling before releasing
        // cache ownership; no pixel readback or texture upload is performed.
        gl->glFinish();
        if (texture) {
            gl->glDeleteTextures(1, &texture);
            texture = 0;
        }
        if (imported != EGL_NO_IMAGE_KHR) {
            destroy(egl, imported);
            imported = EGL_NO_IMAGE_KHR;
        }
        current.reset();
    }
};
ThumbnailPopup::ThumbnailPopup(QWidget *parent) : QObject(parent) {
    setObjectName("seek-preview-popup");
    view_ = new ThumbnailWindow;
    view_->setObjectName("seek-preview-window");
    // A mapped Wayland tooltip cannot reliably change its anchor. A native
    // child surface can move without hiding/recreating the GPU window.
    parent->winId();
    view_->setParent(parent->windowHandle());
    view_->setFlags(Qt::SubWindow | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                    Qt::WindowDoesNotAcceptFocus);
    view_->resize(320, 180);
    connect(view_, &QOpenGLWindow::frameSwapped, this, [this] {
        if (view_->painted_request && presented)
            presented(view_->painted_request);
    });
}
ThumbnailPopup::~ThumbnailPopup() { delete view_; }
void ThumbnailPopup::display(const amdva::streaming::KeyframePreviewStatus &status,
                             const QPoint &anchor) {
    // Retained pixels belong to the previous request, never to this loading one.
    view_->request = status.image ? status.request : 0;
    if (status.image) {
        image_revision_ = status.deletion_generation;
        view_->image(status.image);
    } else if (status.state != "loading" || image_revision_ != status.deletion_generation) {
        view_->image({});
    }
    // With no image yet (or a gap/error), there is nothing to show.
    if (!view_->pending) {
        view_->hide();
        return;
    }
    auto *host = view_->parent();
    const auto local = host->mapFromGlobal(anchor);
    view_->setPosition(
        std::clamp(local.x() - view_->width() / 2, 0, std::max(0, host->width() - view_->width())),
        std::clamp(local.y() - view_->height() - 6, 0,
                   std::max(0, host->height() - view_->height())));
    view_->show();
    view_->raise();
    view_->update();
}
void ThumbnailPopup::clear() {
    view_->hide();
    view_->image({});
}
std::string ThumbnailPopup::error() const { return view_->error; }
bool ThumbnailPopup::exposed() const { return view_->isExposed(); }
bool ThumbnailPopup::isVisible() const { return view_->isVisible(); }
std::shared_ptr<const GpuPreviewImage> ThumbnailPopup::displayed_image() const {
    return view_->pending;
}
} // namespace amdva::nvr
