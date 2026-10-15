#pragma once
#include "amdva/resource_monitor.hpp"
#include <deque>
#include <gst/gst.h>
#include <gst/va/gstva.h>
#include <memory>
#include <string>
namespace amdva::dashboard {
class Renderer {
  public:
    Renderer(const std::string &render_node, const std::string &font, unsigned fps,
             double power_max);
    ~Renderer();
    GstVaDisplay *display() const;
    // Pool-owned NV12 VAMemory: caller takes ownership, no CPU pixel mapping.
    GstBuffer *frame(const std::deque<ResourceSample> &, const std::string &cpu,
                     const std::string &gpu);
    std::string description() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva::dashboard
