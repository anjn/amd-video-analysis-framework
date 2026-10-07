#include "amdva/tensor_meta.hpp"
#include <algorithm>
#include <cmath>
#include <gst/analytics/analytics.h>
namespace amdva {
namespace {
gboolean init(GstMeta *m, gpointer, GstBuffer *) {
    reinterpret_cast<AnalysisMeta *>(m)->data = nullptr;
    return TRUE;
}
void release(GstMeta *m, GstBuffer *) { delete reinterpret_cast<AnalysisMeta *>(m)->data; }
gboolean copy_impl(GstBuffer *out, GstMeta *m, GstBuffer *, GQuark type, gpointer data) {
    const bool merge = type == g_quark_from_static_string("amd-analytics-merge");
    if (!merge &&
        (!GST_META_TRANSFORM_IS_COPY(type) || static_cast<GstMetaTransformCopy *>(data)->region))
        return FALSE;
    auto *meta = reinterpret_cast<AnalysisMeta *>(m);
    if (!meta->data)
        return FALSE;
    // Copies the mutable attachment list; GPU payload owners remain shared.
    auto copied = std::make_shared<AnalysisData>(**meta->data);
    if (merge) {
        const auto &transform = *static_cast<AnalysisMerge *>(data);
        for (auto &attachment : copied->attachments) {
            if (attachment.parent_id >= 0) {
                gpointer id = nullptr;
                if (!g_hash_table_lookup_extended(
                        transform.ids, GUINT_TO_POINTER(attachment.parent_id), nullptr, &id))
                    return FALSE;
                attachment.parent_id = GPOINTER_TO_UINT(id);
            }
            auto &g = attachment.geometry;
            g.original_width = std::lround(g.original_width * transform.scale_x);
            g.original_height = std::lround(g.original_height * transform.scale_y);
            g.crop_x = std::lround(g.crop_x * transform.scale_x);
            g.crop_y = std::lround(g.crop_y * transform.scale_y);
            g.crop_width = std::lround(g.crop_width * transform.scale_x);
            g.crop_height = std::lround(g.crop_height * transform.scale_y);
            for (auto &object : attachment.result.objects) {
                object.box.x *= transform.scale_x;
                object.box.y *= transform.scale_y;
                object.box.w *= transform.scale_x;
                object.box.h *= transform.scale_y;
                for (auto &k : object.keypoints) {
                    k.x *= transform.scale_x;
                    k.y *= transform.scale_y;
                }
            }
        }
    }
    if (auto *existing = get_analysis_meta(out); existing && existing->data) {
        auto &target = **existing->data;
        if (!target.identity.source.empty() && !copied->identity.source.empty() &&
            target.identity.source != copied->identity.source)
            return FALSE;
        if (!target.identity.source.empty() &&
            target.identity.generation != copied->identity.generation)
            return FALSE;
        if (GST_CLOCK_TIME_IS_VALID(target.identity.pts) &&
            GST_CLOCK_TIME_IS_VALID(copied->identity.pts) &&
            target.identity.pts != copied->identity.pts)
            return FALSE;
        auto custom = target.custom_json;
        if (custom.empty())
            custom = copied->custom_json;
        else if (!copied->custom_json.empty() && custom != copied->custom_json) {
            custom = "{\"branches\":[" + custom + "," + copied->custom_json + "]}";
            if (custom.size() > 65536)
                return FALSE;
        }
        target.attachments.insert(target.attachments.end(), copied->attachments.begin(),
                                  copied->attachments.end());
        target.custom_json = std::move(custom);
        if (target.identity.source.empty())
            target.identity = copied->identity;
        return TRUE;
    }
    return add_analysis_meta(out, std::move(copied)) != nullptr;
}
gboolean copy(GstBuffer *out, GstMeta *meta, GstBuffer *source, GQuark type, gpointer data) {
    try {
        return copy_impl(out, meta, source, type, data);
    } catch (...) {
        return FALSE;
    }
}
const GstMetaInfo *info() {
    static gsize value = 0;
    if (g_once_init_enter(&value)) {
        auto *i = gst_meta_register(analysis_meta_api(), "AmdAnalysisMeta", sizeof(AnalysisMeta),
                                    init, release, copy);
        g_once_init_leave(&value, reinterpret_cast<gsize>(i));
    }
    return reinterpret_cast<const GstMetaInfo *>(value);
}
void relate(GstAnalyticsRelationMeta *meta, guint child, int parent) {
    if (parent >= 0 && (!gst_analytics_relation_meta_set_relation(
                            meta, GST_ANALYTICS_REL_TYPE_RELATE_TO, child, parent) ||
                        !gst_analytics_relation_meta_set_relation(
                            meta, GST_ANALYTICS_REL_TYPE_RELATE_TO, parent, child)))
        throw std::runtime_error("Cannot relate analysis metadata");
}
void classification(GstAnalyticsRelationMeta *meta, const std::vector<Classification> &values,
                    const ModelInfo &model, int parent) {
    if (values.empty())
        return;
    std::vector<float> scores;
    std::vector<GQuark> labels;
    for (const auto &c : values) {
        if (c.class_id < 0 || size_t(c.class_id) >= model.labels.size())
            throw std::runtime_error("Invalid classification label");
        scores.push_back(c.score);
        labels.push_back(g_quark_from_string(model.labels[c.class_id].c_str()));
    }
    GstAnalyticsClsMtd cls{};
    if (!gst_analytics_relation_meta_add_cls_mtd(meta, scores.size(), scores.data(), labels.data(),
                                                 &cls))
        throw std::runtime_error("Cannot attach classification");
    relate(meta, cls.id, parent);
}
} // namespace
GType analysis_meta_api() {
    static gsize value = 0;
    if (g_once_init_enter(&value)) {
        const gchar *tags[] = {"amd-analysis", nullptr};
        g_once_init_leave(&value, gst_meta_api_type_register("AmdAnalysisMetaAPI", tags));
    }
    return static_cast<GType>(value);
}
AnalysisMeta *get_analysis_meta(GstBuffer *b) {
    return reinterpret_cast<AnalysisMeta *>(gst_buffer_get_meta(b, analysis_meta_api()));
}
AnalysisMeta *add_analysis_meta(GstBuffer *b, std::shared_ptr<AnalysisData> data) {
    auto *m = reinterpret_cast<AnalysisMeta *>(gst_buffer_add_meta(b, info(), nullptr));
    if (!m)
        return nullptr;
    try {
        m->data = new std::shared_ptr<AnalysisData>(std::move(data));
    } catch (...) {
        gst_buffer_remove_meta(b, &m->meta);
        return nullptr;
    }
    return m;
}
void attach_analysis(GstBuffer *buffer, AnalysisResult result, const ModelInfo &model,
                     const ImageTransform &geometry, const FrameIdentity &identity, int parent) {
    auto *meta = gst_buffer_add_analytics_relation_meta(buffer);
    if (!meta)
        throw std::runtime_error("Cannot attach GstAnalytics metadata");
    classification(meta, result.classifications, model, parent);
    GstAnalyticsClsMtd descriptor{};
    if (!result.objects.empty()) {
        std::vector<GQuark> labels;
        std::vector<float> scores(model.labels.size(), 0);
        for (const auto &l : model.labels)
            labels.push_back(g_quark_from_string(l.c_str()));
        if (!gst_analytics_relation_meta_add_cls_mtd(meta, labels.size(), scores.data(),
                                                     labels.data(), &descriptor))
            throw std::runtime_error("Cannot attach class descriptor");
        gst_analytics_mtd_set_semantic_tag(reinterpret_cast<GstAnalyticsMtd *>(&descriptor),
                                           "class_descriptor");
    }
    for (auto &object : result.objects) {
        auto box = geometry.to_source(object.box.x, object.box.y, object.box.w, object.box.h);
        if (box[2] <= 0 || box[3] <= 0)
            continue;
        if (object.box.class_id < 0 || size_t(object.box.class_id) >= model.labels.size())
            throw std::runtime_error("Invalid object class");
        GstAnalyticsODMtd od{};
        if (!gst_analytics_relation_meta_add_od_mtd(
                meta, g_quark_from_string(model.labels[object.box.class_id].c_str()),
                int(std::floor(box[0])), int(std::floor(box[1])),
                int(std::ceil(box[0] + box[2])) - int(std::floor(box[0])),
                int(std::ceil(box[1] + box[3])) - int(std::floor(box[1])), object.box.score, &od))
            throw std::runtime_error("Cannot attach object");
        relate(meta, od.id, descriptor.id);
        relate(meta, od.id, parent);
        classification(meta, object.classifications, model, od.id);
        if (object.tracking_id) {
            GstAnalyticsTrackingMtd tracking{};
            if (!gst_analytics_relation_meta_add_tracking_mtd(meta, object.tracking_id,
                                                              identity.pts, &tracking))
                throw std::runtime_error("Cannot attach tracking ID");
            relate(meta, tracking.id, od.id);
        }
        if (!object.keypoints.empty()) {
            GstAnalyticsGroupMtd group{};
            if (!gst_analytics_relation_meta_add_group_mtd(meta, object.keypoints.size(), &group))
                throw std::runtime_error("Cannot attach pose group");
            gst_analytics_mtd_set_semantic_tag(reinterpret_cast<GstAnalyticsMtd *>(&group),
                                               object.keypoints.size() == 17 ? "body-pose/coco-17"
                                                                             : "keypoints");
            relate(meta, group.id, od.id);
            for (auto &key : object.keypoints) {
                auto point = geometry.to_source(key.x, key.y, 0, 0);
                key.x = point[0];
                key.y = point[1];
                GstAnalyticsKeypointMtd kp{};
                if (!std::isfinite(key.x) || !std::isfinite(key.y) ||
                    !std::isfinite(key.confidence))
                    continue;
                if (!gst_analytics_relation_meta_add_keypoint_mtd(
                        meta, GST_ANALYTICS_KEYPOINT_DIMENSIONS_2D, int(key.x), int(key.y), 0,
                        GST_ANALYTICS_KEYPOINT_VISIBILITY_VISIBLE, key.confidence, &kp) ||
                    !gst_analytics_group_mtd_add_member(&group, kp.id))
                    throw std::runtime_error("Cannot attach keypoint");
            }
        }
        object.box.x = box[0];
        object.box.y = box[1];
        object.box.w = box[2];
        object.box.h = box[3];
    }
    auto *amd = get_analysis_meta(buffer);
    if (!amd) {
        auto data = std::make_shared<AnalysisData>();
        data->identity = identity;
        amd = add_analysis_meta(buffer, data);
    }
    if (!amd || !amd->data)
        throw std::runtime_error("Cannot attach AMD metadata");
    if ((**amd->data).identity.source.empty())
        (**amd->data).identity = identity;
    (**amd->data).attachments.push_back({std::move(result), geometry, parent});
}
} // namespace amdva
