#pragma once
#include "amdva/telemetry.hpp"
#include <gst/gst.h>
namespace amdva {
// Value copies retain the collector, not mutable timestamp storage.
struct FrameTraceMeta {
    GstMeta meta;
    TraceToken *token;
};
GType frame_trace_meta_api();
FrameTraceMeta *get_frame_trace_meta(GstBuffer *);
TraceToken frame_trace(GstBuffer *);
bool set_frame_trace(GstBuffer *, const TraceToken &);
class PipelineTelemetry {
  public:
    explicit PipelineTelemetry(std::shared_ptr<Telemetry>);
    ~PipelineTelemetry();
    PipelineTelemetry(const PipelineTelemetry &) = delete;
    PipelineTelemetry &operator=(const PipelineTelemetry &) = delete;
    // Attach before PLAYING. Destroy adapters only after their pipelines stop.
    void attach_rtsp(GstElement *rtspsrc);
    void attach_rtp(GstPad *before_jitter, GstPad *after_jitter);
    void attach_access_units(GstPad *parser_src);
    void attach_decoder(GstElement *decoder);
    void attach_stage(GstPad *, TimingStage);
    // Separate appsrc pipelines: register the copied/rebased AU before push.
    void submit(GstBuffer *);
    void background(bool);

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace amdva
