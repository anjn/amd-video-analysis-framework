#pragma once
#include "amdva/streaming/playback.hpp"
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
class ThumbnailPopup;
class Tiles final : public QOpenGLWindow {
  public:
    explicit Tiles(Controller &controller, Slot *playback = nullptr);
    void layout(const std::array<std::string, 16> &cells, unsigned count, int focus = -1);
    std::function<void(unsigned, bool)> activated;
    void select_tile(unsigned cell) {
        selected_ = cell;
        update();
    }
    ~Tiles() override;
    std::string error() const { return error_; }
    uint64_t presented() const { return presented_; }
    void playback_time_visible(bool enabled) {
        playback_time_visible_ = enabled;
        update();
    }
    int64_t displayed_playback_time() const { return displayed_playback_time_; }

  protected:
    void initializeGL() override;
    void paintGL() override;
    void exposeEvent(QExposeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  private:
    struct Texture {
        EGLImageKHR image{EGL_NO_IMAGE_KHR};
        GLuint texture{};
        GLsync fence{};
    };
    Controller &controller_;
    Slot *playback_{};
    Slot &slot(unsigned i) { return playback_ ? *playback_ : controller_.gpu_slots()[i]; }
    unsigned slot_count() const { return playback_ ? 1 : 16; }
    std::array<std::string, 16> cells_{};
    unsigned cell_count_{};
    int focus_{-1};
    unsigned selected_{};
    bool playback_time_visible_{true};
    int64_t displayed_playback_time_{};
    unsigned picked(const QPointF &) const;
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
    void draw_playback_time(int64_t ms, float x, float y, float w, float h);
};
class Window final : public QMainWindow {
  public:
    Window(Controller &controller, Options options, std::string config);
    ~Window() override;
    TimelinePlaybackStatus timeline_status() const;
    KeyframePreviewStatus keyframe_preview_status() const;
    ThumbnailPopup *thumbnail_popup() const;
    Tiles *live_view() const { return tiles_; }
    Tiles *recording_view() const;

  private:
    Controller &controller_;
    Options options_;
    std::string config_;
    QTableWidget *table_{};
    Tiles *tiles_{};
    QLabel *summary_{};
    bool updating_{};
    struct Vms;
    std::unique_ptr<Vms> vms_;
    void refresh();
    void persist(bool force = false);
};
void install_shutdown_signals(QCoreApplication &app);
} // namespace amdva::nvr
