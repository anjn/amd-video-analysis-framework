#pragma once
#include "amdva/tensor_meta.hpp"
namespace amdva {
struct PublicationOptions {
    std::string method{"none"}, file_path, address, topic{"amdva/analytics"};
    unsigned queue_limit{256};
    size_t byte_limit{4 * 1024 * 1024};
};
struct PublicationStatus {
    uint64_t accepted{}, delivered{}, dropped{}, errors{};
    size_t queued{}, queue_bytes{};
    std::string last_error;
};
std::string analysis_json(GstBuffer *, const FrameIdentity &fallback = {});
// Bounded, image-free publication. Broker failure never blocks a video thread.
class ResultPublisher {
  public:
    explicit ResultPublisher(PublicationOptions);
    ~ResultPublisher();
    bool publish(std::string json);
    PublicationStatus status() const;
    bool flush(unsigned timeout_ms = 2000);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva
