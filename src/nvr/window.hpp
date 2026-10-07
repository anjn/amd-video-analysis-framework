#pragma once
#include "controller.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QCoreApplication>
#include <QLabel>
#include <QMainWindow>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWindow>
#include <QTableWidget>
namespace amdva::nvr {
class Tiles final : public QOpenGLWindow {
  public:
    explicit Tiles(Controller &controller);
    ~Tiles() override;
    std::string error() const { return error_; }
    uint64_t presented() const { return presented_; }

  protected:
    void initializeGL() override;
    void paintGL() override;
    void exposeEvent(QExposeEvent *) override;

  private:
    struct Texture {
        EGLImageKHR image{EGL_NO_IMAGE_KHR};
        GLuint texture{};
        GLsync fence{};
    };
    Controller &controller_;
    std::array<std::array<Texture, 3>, 16> textures_{};
    std::array<int, 16> current_{};
    GLuint image_program_{}, box_program_{};
    PFNEGLCREATEIMAGEKHRPROC create_{};
    PFNEGLDESTROYIMAGEKHRPROC destroy_{};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_{};
    EGLDisplay egl_{EGL_NO_DISPLAY};
    QOpenGLExtraFunctions *gl_{};
    GLuint buffer_{};
    std::string error_;
    uint64_t presented_{};
    struct PendingSwap {
        unsigned slot, plane;
        TraceToken timing;
    };
    std::vector<PendingSwap> pending_swap_;
    void observe_visibility();
    void vertices(const float *data, unsigned count, unsigned stride);
    void release_current(unsigned slot);
};
class Window final : public QMainWindow {
  public:
    Window(Controller &controller, Options options, std::string config);
    ~Window() override;

  private:
    Controller &controller_;
    Options options_;
    std::string config_;
    QTableWidget *table_{};
    Tiles *tiles_{};
    QLabel *summary_{};
    bool updating_{};
    void refresh();
    void persist();
};
void install_shutdown_signals(QCoreApplication &app);
} // namespace amdva::nvr
