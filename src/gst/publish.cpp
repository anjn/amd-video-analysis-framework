#include "amdva/publishing.hpp"
#include <gst/base/gstbasetransform.h>
namespace {
struct Lock {
    GMutex *mutex;
    explicit Lock(GMutex *m) : mutex(m) { g_mutex_lock(mutex); }
    ~Lock() { g_mutex_unlock(mutex); }
};
} // namespace
typedef struct {
    GstBaseTransform parent;
    GMutex mutex;
    gchar *method, *file, *address, *topic;
    guint limit;
    amdva::ResultPublisher *state;
} AmdMetaPublish;
typedef struct {
    GstBaseTransformClass parent;
} AmdMetaPublishClass;
G_DEFINE_TYPE(AmdMetaPublish, amd_meta_publish, GST_TYPE_BASE_TRANSFORM)
enum { P_NONE, P_METHOD, P_FILE, P_ADDRESS, P_TOPIC, P_LIMIT, P_DELIVERED, P_DROPPED, P_ERRORS };
static void set_property(GObject *o, guint p, const GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdMetaPublish *>(o);
    Lock lock(&s->mutex);
    if (GST_STATE(s) > GST_STATE_READY)
        return;
    gchar **dest = nullptr;
    switch (p) {
    case P_METHOD:
        dest = &s->method;
        break;
    case P_FILE:
        dest = &s->file;
        break;
    case P_ADDRESS:
        dest = &s->address;
        break;
    case P_TOPIC:
        dest = &s->topic;
        break;
    case P_LIMIT:
        s->limit = g_value_get_uint(v);
        return;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
        return;
    }
    g_free(*dest);
    *dest = g_value_dup_string(v);
}
static void get_property(GObject *o, guint p, GValue *v, GParamSpec *spec) {
    auto *s = reinterpret_cast<AmdMetaPublish *>(o);
    Lock lock(&s->mutex);
    switch (p) {
    case P_METHOD:
        g_value_set_string(v, s->method);
        break;
    case P_FILE:
        g_value_set_string(v, s->file);
        break;
    case P_ADDRESS:
        g_value_set_string(v, s->address);
        break;
    case P_TOPIC:
        g_value_set_string(v, s->topic);
        break;
    case P_LIMIT:
        g_value_set_uint(v, s->limit);
        break;
    case P_DELIVERED:
    case P_DROPPED:
    case P_ERRORS: {
        auto status = s->state ? s->state->status() : amdva::PublicationStatus{};
        g_value_set_uint64(v, p == P_DELIVERED ? status.delivered
                              : p == P_DROPPED ? status.dropped
                                               : status.errors);
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, p, spec);
    }
}
static gboolean start(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdMetaPublish *>(t);
    Lock lock(&s->mutex);
    try {
        amdva::PublicationOptions options;
        options.method = s->method ? s->method : "none";
        options.file_path = s->file ? s->file : "";
        options.address = s->address ? s->address : "";
        options.topic = s->topic ? s->topic : "amdva/analytics";
        options.queue_limit = s->limit;
        auto state = std::make_unique<amdva::ResultPublisher>(options);
        delete s->state;
        s->state = state.release();
        return TRUE;
    } catch (const std::exception &e) {
        GST_ELEMENT_ERROR(s, LIBRARY, INIT, ("Result publication initialization failed"),
                          ("%s", e.what()));
        return FALSE;
    }
}
static gboolean stop(GstBaseTransform *t) {
    auto *s = reinterpret_cast<AmdMetaPublish *>(t);
    Lock lock(&s->mutex);
    delete s->state;
    s->state = nullptr;
    return TRUE;
}
static GstFlowReturn transform(GstBaseTransform *t, GstBuffer *buffer) {
    auto *s = reinterpret_cast<AmdMetaPublish *>(t);
    try {
        s->state->publish(amdva::analysis_json(buffer));
        return GST_FLOW_OK;
    } catch (const std::exception &e) {
        GST_ELEMENT_WARNING(s, RESOURCE, WRITE, ("Result serialization failed"), ("%s", e.what()));
        return GST_FLOW_OK;
    }
}
static void finalize(GObject *o) {
    auto *s = reinterpret_cast<AmdMetaPublish *>(o);
    delete s->state;
    g_free(s->method);
    g_free(s->file);
    g_free(s->address);
    g_free(s->topic);
    g_mutex_clear(&s->mutex);
    G_OBJECT_CLASS(amd_meta_publish_parent_class)->finalize(o);
}
static void amd_meta_publish_class_init(AmdMetaPublishClass *k) {
    auto *object = G_OBJECT_CLASS(k);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    auto flags = static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
                                          GST_PARAM_MUTABLE_READY);
    g_object_class_install_property(
        object, P_METHOD,
        g_param_spec_string("method", "Method", "none, file, mqtt or kafka", "none", flags));
    g_object_class_install_property(
        object, P_FILE,
        g_param_spec_string("file-path", "File", "JSON lines destination", nullptr, flags));
    g_object_class_install_property(
        object, P_ADDRESS,
        g_param_spec_string("address", "Address", "Broker URI/address", nullptr, flags));
    g_object_class_install_property(
        object, P_TOPIC,
        g_param_spec_string("topic", "Topic", "Broker topic", "amdva/analytics", flags));
    g_object_class_install_property(
        object, P_LIMIT,
        g_param_spec_uint("queue-limit", "Queue", "Maximum queued records", 1, 65536, 256, flags));
    for (auto item : {std::pair<unsigned, const char *>{P_DELIVERED, "delivered"},
                      {P_DROPPED, "dropped"},
                      {P_ERRORS, "errors"}})
        g_object_class_install_property(
            object, item.first,
            g_param_spec_uint64(
                item.second, item.second, "Publication counter", 0, G_MAXUINT64, 0,
                static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));
    auto *element = GST_ELEMENT_CLASS(k);
    gst_element_class_set_static_metadata(
        element, "AMD bounded result publication", "Filter/Metadata",
        "Publishes compact results independently of video backpressure", "AMD");
    gst_element_class_add_pad_template(
        element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, gst_caps_new_any()));
    gst_element_class_add_pad_template(
        element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, gst_caps_new_any()));
    auto *base = GST_BASE_TRANSFORM_CLASS(k);
    base->start = start;
    base->stop = stop;
    base->transform_ip = transform;
}
static void amd_meta_publish_init(AmdMetaPublish *s) {
    g_mutex_init(&s->mutex);
    s->method = g_strdup("none");
    s->topic = g_strdup("amdva/analytics");
    s->limit = 256;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(s), TRUE);
}
