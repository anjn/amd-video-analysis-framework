#pragma once
#include "amdva/streaming/playback.hpp"
#include "controller.hpp"
#include "vector_text.hpp"
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
    std::function<void()> selection_started;
    std::function<void(streaming::SearchImage)> region_selected;
    std::function<void(std::string)> selection_error;
    unsigned search_image_size{384}, search_device{};
    void cancel_selection();
    bool selection_busy() const { return extracting_.valid(); }
    QRectF displayed_video_rect() const { return video_rect_; }
    std::string error() const { return error_; }
    uint64_t presented() const { return presented_; }
    void playback_time_visible(bool enabled) {
        playback_time_visible_ = enabled;
        update();
    }
    int64_t displayed_playback_time() const { return displayed_playback_time_; }
    void stream_info_visible(bool enabled) {
        stream_info_visible_ = enabled;
        update();
    }
    const std::array<QString, 16> &displayed_stream_info() const { return displayed_stream_info_; }

  protected:
    void initializeGL() override;
    void paintGL() override;
    void exposeEvent(QExposeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  private:
    struct Texture {
        EGLImageKHR image{EGL_NO_IMAGE_KHR};
        GLuint texture{};
        GLsync fence{};
    };
    bool frozen_{}, dragging_{}, selection_cancelled_{};
    QPointF selection_start_, selection_end_;
    QRectF video_rect_;
    std::future<streaming::SearchImage> extracting_;
    void poll_selection();
    Controller &controller_;
    Slot *playback_{};
    Slot &slot(unsigned i) { return playback_ ? *playback_ : controller_.gpu_slots()[i]; }
    unsigned slot_count() const { return playback_ ? 1 : 16; }
    std::array<std::string, 16> cells_{};
    unsigned cell_count_{};
    int focus_{-1};
    unsigned selected_{};
    bool playback_time_visible_{true}, stream_info_visible_{true};
    VectorText overlay_font_;
    std::array<QString, 16> displayed_stream_info_{};
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
    void draw_text_overlay(const QString &, float x, float y, float w, float h,
                           bool bottom = false);
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
