#pragma once
#include "amdva/model.hpp"
#include "amdva/runtime.hpp"
#include <array>
#include <functional>
#include <future>
#include <gst/va/gstvadisplay.h>
namespace amdva::streaming {
class Catalog;
struct Frame;
struct SearchOptions {
    bool enabled{};
    std::string library{"libeg2.so"}, bundle;
    unsigned device{}, image_size{384}, interval_ms{1000}, queue_limit{64};
};
struct SearchImage {
    unsigned width{}, height{};
    std::vector<uint8_t> rgb;
};
struct SearchObservation {
    int64_t id{}, time_ms{};
    std::string channel, session, camera_name, recording, model;
    Detection box;
    std::vector<uint8_t> png;
    std::array<float, 768> embedding{};
    double similarity{};
};
struct SearchGroup {
    std::string channel, camera_name;
    int64_t from_ms{}, to_ms{};
    double similarity{};
    std::vector<SearchObservation> images;
};
struct SearchResult {
    uint64_t request{};
    std::string error;
    std::vector<SearchGroup> groups;
};
struct SearchStatus {
    std::string state{"disabled"}, error;
    uint64_t stored{}, dropped{}, pending{};
    double inference_ms{};
};
Detection search_crop(Detection box, unsigned width, unsigned height, float margin = 0);
std::pair<unsigned, unsigned> search_dimensions(unsigned width, unsigned height, unsigned limit);
std::vector<uint8_t> search_letterbox(const SearchImage &, unsigned size);
std::vector<SearchGroup> group_search_results(std::vector<SearchObservation>);
// GPU crop/resize with bounded RGB readback; no CPU mapping of source video.
class SearchCropper {
  public:
    SearchCropper(GstVaDisplay *, unsigned limit, unsigned device = 0);
    ~SearchCropper();
    SearchImage crop(VASurfaceID, unsigned width, unsigned height, Detection);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class SearchService {
  public:
    SearchService(Catalog &, GstVaDisplay *, SearchOptions);
    ~SearchService();
    void submit(const Frame &, const std::string &channel, const std::vector<Detection> &);
    uint64_t text(std::string);
    uint64_t image(SearchImage);
    void cancel();
    SearchResult result() const;
    SearchStatus status() const;
    void shutdown();
    void wait_initialized();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace amdva::streaming
