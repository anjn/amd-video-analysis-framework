#include "amdva/rtsp_server.hpp"
#include <memory>
#include <stdexcept>

namespace amdva {
namespace {
using ConfigureHolder = std::shared_ptr<RtspServer::Configure>;
void configure_media(GstRTSPMediaFactory *, GstRTSPMedia *media, gpointer data) {
    auto &holder = *static_cast<ConfigureHolder *>(data);
    auto *pipeline = gst_rtsp_media_get_element(media);
    (*holder)(media, pipeline);
    gst_object_unref(pipeline);
}
} // namespace

RtspServer::RtspServer(const Options &options) : options_(options), server_(gst_rtsp_server_new()) {
    g_object_set(server_, "address", options.address.c_str(), "service",
                 std::to_string(options.port).c_str(), nullptr);
}
RtspServer::~RtspServer() {
    if (source_)
        g_source_remove(source_);
    gst_object_unref(server_);
}
void RtspServer::add_mount(const std::string &path, const std::string &launch,
                           Configure configure) {
    if (path.size() < 2 || path.front() != '/' || path.find_first_of(" \t\n?#") != std::string::npos)
        throw std::runtime_error("Invalid RTSP mount " + path);
    auto *factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_media_factory_set_launch(factory, launch.c_str());
    if (configure) {
        auto *holder = new ConfigureHolder(std::make_shared<Configure>(std::move(configure)));
        g_signal_connect_data(
            factory, "media-configure", G_CALLBACK(configure_media), holder,
            [](gpointer p, GClosure *) { delete static_cast<ConfigureHolder *>(p); },
            GConnectFlags(0));
    }
    auto *mounts = gst_rtsp_server_get_mount_points(server_);
    gst_rtsp_mount_points_add_factory(mounts, path.c_str(), factory);
    gst_object_unref(mounts);
}
void RtspServer::attach() {
    source_ = gst_rtsp_server_attach(server_, nullptr);
    if (!source_)
        throw std::runtime_error("Cannot bind RTSP listener " + options_.address + ":" +
                                 std::to_string(options_.port));
}
void RtspServer::disconnect_clients() {
    auto *clients = gst_rtsp_server_client_filter(
        server_,
        [](GstRTSPServer *, GstRTSPClient *client, gpointer) -> GstRTSPFilterResult {
            gst_rtsp_client_close(client);
            return GST_RTSP_FILTER_REMOVE;
        },
        nullptr);
    g_list_free_full(clients, g_object_unref);
    auto *pool = gst_rtsp_server_get_session_pool(server_);
    auto *sessions = gst_rtsp_session_pool_filter(
        pool,
        [](GstRTSPSessionPool *, GstRTSPSession *, gpointer) -> GstRTSPFilterResult {
            return GST_RTSP_FILTER_REMOVE;
        },
        nullptr);
    g_list_free_full(sessions, g_object_unref);
    gst_object_unref(pool);
}
std::string RtspServer::url(const std::string &path) const {
    auto host = options_.address == "0.0.0.0" ? "127.0.0.1"
                : options_.address == "::"    ? "::1"
                                              : options_.address;
    if (host.find(':') != std::string::npos)
        host = "[" + host + "]";
    return "rtsp://" + host + ":" + std::to_string(options_.port) + path;
}
} // namespace amdva
