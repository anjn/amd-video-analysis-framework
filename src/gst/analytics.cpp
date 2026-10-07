#include "amdva/batching.hpp"
#include "amdva/video_preprocess.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <gst/analytics/analytics.h>
#include <gst/base/gstbasetransform.h>
#include <map>

struct AnalyticsState {
    std::shared_ptr<amdva::SharedBatchExecutor> batch;
    std::unique_ptr<amdva::VideoPreprocessor> preproc;
    GstVideoInfo video{};
    std::string source;
    std::atomic<uint64_t> generation{0}, sequence{0};
    struct Stream {
        std::string id;
        uint64_t generation{}, sequence{};
        std::chrono::steady_clock::time_point last_seen{};
    };
    std::map<unsigned, Stream> streams;
    uint64_t next_stream_generation{};
    uint64_t preprocessing_epoch{~uint64_t(0)};
    bool container{};
    std::map<std::string, std::unique_ptr<amdva::VideoPreprocessor>> stream_preproc;
};
typedef struct {
    GstBaseTransform parent;
    gchar *model, *info, *instance, *region;
    gint device;
    guint batch, timeout, interval;
    gfloat confidence, nms;
    gboolean strict;
    AnalyticsState *state;
} AmdAnalytics;
typedef struct {
    GstBaseTransformClass parent;
} AmdAnalyticsClass;
G_DEFINE_TYPE(AmdAnalytics, amd_analytics, GST_TYPE_BASE_TRANSFORM)
// Separate GTypes make task defaults explicit while sharing one implementation.
typedef struct {
    AmdAnalytics parent;
} AmdDetect;
typedef struct {
    AmdAnalyticsClass parent;
} AmdDetectClass;
typedef struct {
    AmdAnalytics parent;
} AmdClassify;
typedef struct {
    AmdAnalyticsClass parent;
} AmdClassifyClass;
typedef struct {
    AmdAnalytics parent;
} AmdInference;
typedef struct {
    AmdAnalyticsClass parent;
} AmdInferenceClass;
G_DEFINE_TYPE(AmdDetect, amd_detect, amd_analytics_get_type())
G_DEFINE_TYPE(AmdClassify, amd_classify, amd_analytics_get_type())
G_DEFINE_TYPE(AmdInference, amd_inference, amd_analytics_get_type())
enum {
    P_NONE,
    P_MODEL,
    P_INFO,
    P_INSTANCE,
    P_DEVICE,
    P_BATCH,
    P_TIMEOUT,
    P_INTERVAL,
    P_CONFIDENCE,
    P_NMS,
    P_REGION,
    P_STRICT
};
static GstStaticPadTemplate sink = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766];multistream/"
        "x-analytics-batch(meta:GstAnalyticsBatchMeta)"));
static GstStaticPadTemplate src = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766];multistream/"
        "x-analytics-batch(meta:GstAnalyticsBatchMeta)"));
static void set_property(GObject *o, guint p, const GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdAnalytics *>(o);
    if (GST_STATE(s) > GST_STATE_READY) {
        GST_WARNING_OBJECT(s, "Properties require NULL/READY");
        return;
    }
    switch (p) {
    case P_MODEL:
        g_free(s->model);
        s->model = g_value_dup_string(v);
        break;
    case P_INFO:
        g_free(s->info);
        s->info = g_value_dup_string(v);
        break;
    case P_INSTANCE:
        g_free(s->instance);
        s->instance = g_value_dup_string(v);
        break;
    case P_REGION:
        g_free(s->region);
        s->region = g_value_dup_string(v);
        break;
    case P_DEVICE:
        s->device = g_value_get_int(v);
        break;
    case P_BATCH:
        s->batch = g_value_get_uint(v);
        break;
    case P_TIMEOUT:
        s->timeout = g_value_get_uint(v);
        break;
    case P_INTERVAL:
        s->interval = g_value_get_uint(v);
        break;
    case P_CONFIDENCE:
        s->confidence = g_value_get_float(v);
        break;
    case P_NMS:
        s->nms = g_value_get_float(v);
        break;
    case P_STRICT:
        s->strict = g_value_get_boolean(v);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
    }
}
static void get_property(GObject *o, guint p, GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdAnalytics *>(o);
    switch (p) {
    case P_MODEL:
        g_value_set_string(v, s->model);
        break;
    case P_INFO:
        g_value_set_string(v, s->info);
        break;
    case P_INSTANCE:
        g_value_set_string(v, s->instance);
        break;
    case P_REGION:
        g_value_set_string(v, s->region);
        break;
    case P_DEVICE:
        g_value_set_int(v, s->device);
        break;
    case P_BATCH:
        g_value_set_uint(v, s->batch);
        break;
    case P_TIMEOUT:
        g_value_set_uint(v, s->timeout);
        break;
    case P_INTERVAL:
        g_value_set_uint(v, s->interval);
        break;
    case P_CONFIDENCE:
        g_value_set_float(v, s->confidence);
        break;
    case P_NMS:
        g_value_set_float(v, s->nms);
        break;
    case P_STRICT:
        g_value_set_boolean(v, s->strict);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
    }
}
static gboolean start(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdAnalytics *>(t);
    try {
        if (!s->strict || !s->model || !s->info)
            throw std::runtime_error("Strict GPU mode, model and model-info are required");
        if (g_strcmp0(s->region, "full-frame") && g_strcmp0(s->region, "roi-list"))
            throw std::runtime_error("inference-region must be full-frame or roi-list");
        auto state = std::make_unique<AnalyticsState>();
        amdva::BatchOptions options;
        options.model = s->model;
        options.model_info = s->info;
        options.instance_id = s->instance ? s->instance : "";
        options.device = s->device;
        options.size = s->batch;
        options.timeout_ms = s->timeout;
        options.confidence = s->confidence;
        options.nms = s->nms;
        options.raw_outputs = false;
        state->batch = amdva::SharedBatchExecutor::acquire(options);
        const auto &info = state->batch->info();
        if (G_TYPE_CHECK_INSTANCE_TYPE(s, amd_detect_get_type()) &&
            (info.task != "detection" && info.task != "instance-segmentation" &&
             info.task != "pose"))
            throw std::runtime_error(
                "amddetect requires a detection, instance-segmentation or pose model");
        if (G_TYPE_CHECK_INSTANCE_TYPE(s, amd_classify_get_type()) && info.task != "classification")
            throw std::runtime_error("amdclassify requires classification logits");
        static std::atomic<uint64_t> identity{0};
        state->source = "amd-analytics/" + std::to_string(++identity);
        delete s->state;
        s->state = state.release();
        return TRUE;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(s, LIBRARY, INIT, ("AMD analytics initialization failed"),
                          ("%s", e.what()));
        return FALSE;
    }
}
static gboolean stop(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdAnalytics *>(t);
    if (s->state)
        s->state->batch->release_source(s->state->source,
                                        (s->state->generation.load() << 32) | 0xffffffffULL);
    delete s->state;
    s->state = nullptr;
    return TRUE;
}
static gboolean set_caps(GstBaseTransform *t, GstCaps *in, GstCaps *) {
    auto *s = reinterpret_cast<AmdAnalytics *>(t);
    if (!s->state)
        return FALSE;
    s->state->container =
        gst_structure_has_name(gst_caps_get_structure(in, 0), "multistream/x-analytics-batch");
    return s->state->container || gst_video_info_from_caps(&s->state->video, in);
}
static gboolean event(GstBaseTransform *t, GstEvent *event) {
    auto *s = reinterpret_cast<AmdAnalytics *>(t);
    if (s->state && (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START ||
                     GST_EVENT_TYPE(event) == GST_EVENT_STREAM_START)) {
        s->state->batch->cancel(s->state->source,
                                (s->state->generation.load() << 32) | 0xffffffffULL);
        ++s->state->generation;
        s->state->sequence = 0;
    }
    return GST_BASE_TRANSFORM_CLASS(amd_analytics_parent_class)->sink_event(t, event);
}
static GstFlowReturn transform(GstBaseTransform *t, GstBuffer *input) {
    auto *s = reinterpret_cast<AmdAnalytics *>(t);
    const auto epoch = s->state->generation.load();
    try {
        auto &state = *s->state;
        const auto &info = state.batch->info();
        if (state.preprocessing_epoch != epoch) {
            state.preproc.reset();
            state.stream_preproc.clear();
            state.streams.clear();
            if (!state.container)
                state.preproc = std::make_unique<amdva::VideoPreprocessor>(info, s->device);
            state.preprocessing_epoch = epoch;
        }
        struct Pending {
            GstBuffer *buffer;
            amdva::ImageTransform geometry;
            amdva::FrameIdentity identity;
            int parent;
        };
        std::vector<Pending> pending;
        std::vector<amdva::InferenceRequest> requests;
        auto drain = [&] {
            if (requests.empty())
                return;
            auto futures = state.batch->submit_many(std::move(requests));
            requests.clear();
            for (unsigned i = 0; i < futures.size(); i++) {
                auto result = futures[i].get();
                if (state.generation.load() != epoch)
                    continue;
                auto &p = pending[i];
                amdva::attach_analysis(p.buffer, std::move(result), info, p.geometry, p.identity,
                                       p.parent);
            }
            pending.clear();
        };
        auto frame = [&](GstBuffer *buffer, const GstVideoInfo &video, const std::string &source,
                         uint64_t generation, uint64_t sequence) {
            if (sequence % s->interval)
                return;
            struct Roi {
                int parent;
                unsigned x, y, w, h;
            };
            std::vector<Roi> rois;
            if (g_strcmp0(s->region, "roi-list") == 0) {
                auto *meta = gst_buffer_get_analytics_relation_meta(buffer);
                gpointer cursor = nullptr;
                GstAnalyticsMtd m{};
                while (meta && gst_analytics_relation_meta_iterate(
                                   meta, &cursor, gst_analytics_od_mtd_get_mtd_type(), &m)) {
                    gint x, y, w, h;
                    gfloat confidence;
                    gst_analytics_od_mtd_get_location(reinterpret_cast<GstAnalyticsODMtd *>(&m), &x,
                                                      &y, &w, &h, &confidence);
                    const int left = std::clamp(x, 0, video.width),
                              top = std::clamp(y, 0, video.height);
                    const int right =
                                  int(std::clamp(int64_t(x) + w, int64_t(0), int64_t(video.width))),
                              bottom = int(
                                  std::clamp(int64_t(y) + h, int64_t(0), int64_t(video.height)));
                    if (right > left && bottom > top)
                        rois.push_back({int(m.id), unsigned(left), unsigned(top),
                                        unsigned(right - left), unsigned(bottom - top)});
                    if (rois.size() > 300)
                        throw std::runtime_error("ROI budget exceeded");
                }
            } else
                rois.push_back({-1, 0, 0, unsigned(video.width), unsigned(video.height)});
            amdva::FrameIdentity identity{source, generation, sequence, GST_BUFFER_PTS(buffer)};
            if (state.container) {
                for (const auto &[index, stream] : state.streams)
                    if (source == state.source + "/stream/" + std::to_string(index) &&
                        !stream.id.empty()) {
                        identity.source = stream.id;
                        break;
                    }
            }
            if (!state.container) {
                auto *id = gst_pad_get_stream_id(GST_BASE_TRANSFORM_SINK_PAD(t));
                if (id) {
                    identity.source = id;
                    g_free(id);
                }
            }
            if (auto *m = amdva::get_analysis_meta(buffer); m && m->data) {
                if (!(**m->data).identity.source.empty())
                    identity = (**m->data).identity;
                else
                    (**m->data).identity = identity;
            }
            for (unsigned i = 0; i < rois.size(); i++) {
                const auto &roi = rois[i];
                auto *processor = state.preproc.get();
                if (state.container) {
                    auto &per_stream = state.stream_preproc[source];
                    if (!per_stream)
                        per_stream = std::make_unique<amdva::VideoPreprocessor>(info, s->device);
                    processor = per_stream.get();
                }
                auto tensor = processor->prepare(buffer, video, roi.x, roi.y, roi.w, roi.h);
                amdva::InferenceRequest request;
                request.source = source + (roi.parent < 0 ? "" : "/roi/" + std::to_string(i));
                request.generation = generation;
                request.sequence = sequence;
                request.tensor = {info.input_name,
                                  amdva::TensorType::Float32,
                                  {1, 3, info.height, info.width},
                                  tensor->buffer,
                                  s->device};
                requests.push_back(std::move(request));
                pending.push_back({buffer, tensor->geometry, identity, roi.parent});
                if (requests.size() == 16)
                    drain();
            }
            if (auto *meta = amdva::get_tensor_meta(buffer))
                gst_buffer_remove_meta(buffer, &meta->meta);
        };
        if (state.container) {
            const auto now = std::chrono::steady_clock::now();
            for (auto it = state.streams.begin(); it != state.streams.end();)
                if (now - it->second.last_seen > std::chrono::seconds(60)) {
                    const auto source = state.source + "/stream/" + std::to_string(it->first);
                    state.batch->release_source(source, (epoch << 32) | it->second.generation);
                    state.stream_preproc.erase(source);
                    it = state.streams.erase(it);
                } else
                    ++it;
            auto *batch = gst_buffer_get_analytics_batch_meta(input);
            if (!batch || batch->n_streams > 256)
                throw std::runtime_error("Invalid analytics batch container");
            for (unsigned i = 0; i < batch->n_streams; i++) {
                auto &stream = batch->streams[i];
                if (!state.streams.count(stream.index) && state.streams.size() >= 256)
                    throw std::runtime_error("Stream identity budget exceeded");
                auto &previous = state.streams[stream.index];
                previous.last_seen = now;
                const auto source = state.source + "/stream/" + std::to_string(stream.index);
                const auto *id = gst_analytics_batch_stream_get_stream_id(&stream);
                const std::string current = id ? id : "";
                if (!previous.generation || previous.id != current) {
                    state.batch->cancel(source, (epoch << 32) | previous.generation);
                    previous.id = current;
                    if (state.next_stream_generation >= 0xffffffffULL)
                        throw std::runtime_error("Stream generation exhausted");
                    previous.generation = ++state.next_stream_generation;
                    previous.sequence = 0;
                    state.stream_preproc.erase(source);
                }
                auto *caps = gst_analytics_batch_stream_get_caps(&stream);
                GstVideoInfo video{};
                if (!caps || !gst_video_info_from_caps(&video, caps))
                    throw std::runtime_error("Batch stream has no video caps");
                for (unsigned j = 0; j < stream.n_objects; j++)
                    if (GST_IS_BUFFER(stream.objects[j])) {
                        auto *buffer = gst_buffer_make_writable(GST_BUFFER_CAST(stream.objects[j]));
                        stream.objects[j] = GST_MINI_OBJECT(buffer);
                        frame(buffer, video, source, (epoch << 32) | previous.generation,
                              previous.sequence++);
                    }
            }
        } else
            frame(input, state.video, state.source, epoch << 32, state.sequence++);
        drain();
        return state.generation.load() == epoch ? GST_FLOW_OK : GST_FLOW_FLUSHING;
    } catch (const std::exception &e) {
        if (s->state->generation.load() != epoch ||
            GST_PAD_IS_FLUSHING(GST_BASE_TRANSFORM_SINK_PAD(t)))
            return GST_FLOW_FLUSHING;
        GST_ELEMENT_ERROR(s, STREAM, FAILED, ("AMD analytics failed"), ("%s", e.what()));
        return GST_FLOW_ERROR;
    }
}
static void finalize(GObject *o) {
    auto *s = reinterpret_cast<AmdAnalytics *>(o);
    delete s->state;
    g_free(s->model);
    g_free(s->info);
    g_free(s->instance);
    g_free(s->region);
    G_OBJECT_CLASS(amd_analytics_parent_class)->finalize(o);
}
static void amd_analytics_class_init(AmdAnalyticsClass *k) {
    auto *object = G_OBJECT_CLASS(k);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                          GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(
        object, P_MODEL,
        g_param_spec_string("model", "Model", "Device-I/O MXR model", nullptr, flags));
    g_object_class_install_property(object, P_INFO,
                                    g_param_spec_string("model-info", "Model info",
                                                        "Preprocessing/postprocessing JSON",
                                                        nullptr, flags));
    g_object_class_install_property(object, P_INSTANCE,
                                    g_param_spec_string("model-instance-id", "Instance",
                                                        "Shared executor identity", nullptr,
                                                        flags));
    g_object_class_install_property(object, P_REGION,
                                    g_param_spec_string("inference-region", "Region",
                                                        "full-frame or roi-list", "full-frame",
                                                        flags));
    g_object_class_install_property(
        object, P_DEVICE, g_param_spec_int("device", "Device", "HIP device", 0, 16, 0, flags));
    g_object_class_install_property(
        object, P_BATCH,
        g_param_spec_uint("batch-size", "Batch", "Compiled model batch size", 1, 16, 1, flags));
    g_object_class_install_property(object, P_TIMEOUT,
                                    g_param_spec_uint("batch-timeout", "Timeout",
                                                      "Partial batch wait in milliseconds", 0,
                                                      10000, 20, flags));
    g_object_class_install_property(object, P_INTERVAL,
                                    g_param_spec_uint("inference-interval", "Interval",
                                                      "Infer every Nth frame", 1, 100000, 1,
                                                      flags));
    g_object_class_install_property(object, P_CONFIDENCE,
                                    g_param_spec_float("confidence-threshold", "Confidence",
                                                       "Minimum object score", 0, 1, .25f, flags));
    g_object_class_install_property(
        object, P_NMS,
        g_param_spec_float("nms-threshold", "NMS", "NMS IoU threshold", 0, 1, .45f, flags));
    g_object_class_install_property(object, P_STRICT,
                                    g_param_spec_boolean("require-zero-copy", "Strict",
                                                         "Require GPU image/tensor processing",
                                                         TRUE, flags));
    auto *element = GST_ELEMENT_CLASS(k);
    gst_element_class_set_static_metadata(element, "AMD video analytics", "Filter/Video",
                                          "Shared GPU inference with GstAnalytics metadata", "AMD");
    gst_element_class_add_static_pad_template(element, &sink);
    gst_element_class_add_static_pad_template(element, &src);
    auto *base = GST_BASE_TRANSFORM_CLASS(k);
    base->start = start;
    base->stop = stop;
    base->set_caps = set_caps;
    base->transform_ip = transform;
    base->sink_event = event;
}
static void amd_analytics_init(AmdAnalytics *s) {
    s->batch = 1;
    s->timeout = 20;
    s->interval = 1;
    s->confidence = .25f;
    s->nms = .45f;
    s->strict = TRUE;
    s->region = g_strdup("full-frame");
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(s), TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(s), FALSE);
}
static void amd_detect_class_init(AmdDetectClass *) {}
static void amd_detect_init(AmdDetect *) {}
static void amd_classify_class_init(AmdClassifyClass *) {}
static void amd_classify_init(AmdClassify *p) {
    auto *s = reinterpret_cast<AmdAnalytics *>(p);
    g_free(s->region);
    s->region = g_strdup("roi-list");
}
static void amd_inference_class_init(AmdInferenceClass *) {}
static void amd_inference_init(AmdInference *) {}
