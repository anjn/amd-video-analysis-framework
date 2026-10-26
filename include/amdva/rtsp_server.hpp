#pragma once
#include <functional>
#include <gst/rtsp-server/rtsp-server.h>
#include <string>

namespace amdva {
// Thin owner of a GstRTSPServer for appsrc-fed streams. Every mount point is a
// shared media factory; the launch description must name its source `video` and
// its payloader `pay0`, for example
//   ( appsrc name=video ! queue ! vah264enc ! h264parse ! rtph264pay name=pay0 pt=96 )
// The server attaches to the default main context; run a GMainLoop to serve.
class RtspServer {
  public:
    struct Options {
        std::string address{"0.0.0.0"};
        unsigned port{8554};
    };
    // Called from the RTSP thread when a client first prepares the mount point.
    using Configure = std::function<void(GstRTSPMedia *media, GstElement *pipeline)>;
    explicit RtspServer(const Options &options);
    ~RtspServer();
    RtspServer(const RtspServer &) = delete;
    RtspServer &operator=(const RtspServer &) = delete;
    void add_mount(const std::string &path, const std::string &launch, Configure configure = {});
    // Binds the listener. Throws std::runtime_error when the address is unusable.
    void attach();
    // Closes every client and removes every session; the listener stays attached.
    void disconnect_clients();
    // URL clients can use for a mount point (0.0.0.0 and :: map to loopback).
    std::string url(const std::string &path) const;
    GstRTSPServer *handle() const { return server_; }

  private:
    Options options_;
    GstRTSPServer *server_{};
    guint source_{};
};
} // namespace amdva
