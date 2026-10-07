#pragma once
#include "amdva/runtime.hpp"
#include "amdva/model.hpp"
#include <gst/gst.h>
#include <memory>
namespace amdva {
struct Tensor {
    int device{};
    unsigned width{640},height{640},original_width{},original_height{};
    unsigned scaled_width{},scaled_height{},pad_x{},pad_y{};
    std::shared_ptr<DeviceBuffer> buffer;
    hipEvent_t ready{};
    ImageTransform geometry;
    ~Tensor() { if(ready) { (void)hipSetDevice(device); (void)hipEventSynchronize(ready); (void)hipEventDestroy(ready); } }
};
struct TensorMeta { GstMeta meta; std::shared_ptr<Tensor>* tensor; };
GType tensor_meta_api();
const GstMetaInfo* tensor_meta_info();
TensorMeta* add_tensor_meta(GstBuffer* buffer, std::shared_ptr<Tensor> tensor);
TensorMeta* get_tensor_meta(GstBuffer* buffer);
struct FrameIdentity {
    std::string source;
    uint64_t generation{}, sequence{};
    GstClockTime pts{GST_CLOCK_TIME_NONE};
    bool background{};
};
struct AnalysisAttachment {
    AnalysisResult result;
    ImageTransform geometry;
    int parent_id{-1};
};
struct AnalysisData {
    FrameIdentity identity;
    std::vector<AnalysisAttachment> attachments;
    std::string custom_json;
};
struct AnalysisMeta { GstMeta meta; std::shared_ptr<AnalysisData>* data; };
// Optional metadata transform used by the selected DL Streamer aggregator.
// The ID map is borrowed for the duration of the transform callback.
struct AnalysisMerge { GHashTable* ids; float scale_x,scale_y; };
GType analysis_meta_api();
AnalysisMeta* get_analysis_meta(GstBuffer*);
AnalysisMeta* add_analysis_meta(GstBuffer*, std::shared_ptr<AnalysisData>);
// Attaches standard GstAnalytics metadata and GPU-backed AMD attachments.
void attach_analysis(GstBuffer*, AnalysisResult, const ModelInfo&, const ImageTransform&,
                     const FrameIdentity&, int parent_id = -1);
} // namespace amdva
