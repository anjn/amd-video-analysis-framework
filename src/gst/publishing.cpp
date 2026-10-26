#include "amdva/publishing.hpp"
#ifdef AMDVA_HAVE_MQTT
#include <MQTTAsync.h>
#else
using MQTTAsync = void *; // Not built: the client handle stays null.
#endif
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <gst/analytics/analytics.h>
#include <json-glib/json-glib.h>
#ifdef AMDVA_HAVE_KAFKA
#include <librdkafka/rdkafka.h>
#else
typedef struct rd_kafka_s rd_kafka_t; // Not built: the producer stays null.
#endif
#include <thread>
namespace amdva {
std::string analysis_json(GstBuffer *buffer, const FrameIdentity &fallback) {
    auto identity = fallback;
    auto *amd = get_analysis_meta(buffer);
    if (amd && amd->data)
        identity = (**amd->data).identity;
    auto *root = json_object_new();
    json_object_set_int_member(root, "schema", 1);
    json_object_set_string_member(root, "source", identity.source.c_str());
    json_object_set_int_member(root, "generation", identity.generation);
    json_object_set_int_member(root, "sequence", identity.sequence);
    const auto pts = GST_CLOCK_TIME_IS_VALID(identity.pts) ? identity.pts : GST_BUFFER_PTS(buffer);
    if (GST_CLOCK_TIME_IS_VALID(pts))
        json_object_set_int_member(root, "pts_ns", pts);
    json_object_set_boolean_member(root, "background", identity.background);
    auto *objects = json_array_new();
    auto *meta = gst_buffer_get_analytics_relation_meta(buffer);
    gpointer cursor = nullptr;
    GstAnalyticsMtd item{};
    while (meta && gst_analytics_relation_meta_iterate(
                       meta, &cursor, gst_analytics_od_mtd_get_mtd_type(), &item)) {
        auto *od = reinterpret_cast<GstAnalyticsODMtd *>(&item);
        gint x, y, w, h;
        gfloat score;
        gst_analytics_od_mtd_get_location(od, &x, &y, &w, &h, &score);
        auto *object = json_object_new();
        auto *label = g_quark_to_string(gst_analytics_od_mtd_get_obj_type(od));
        json_object_set_string_member(object, "label", label ? label : "");
        json_object_set_double_member(object, "confidence", score);
        json_object_set_int_member(object, "x", x);
        json_object_set_int_member(object, "y", y);
        json_object_set_int_member(object, "width", w);
        json_object_set_int_member(object, "height", h);
        gpointer related = nullptr;
        GstAnalyticsMtd tracking{};
        if (gst_analytics_relation_meta_get_direct_related(
                meta, item.id, GST_ANALYTICS_REL_TYPE_ANY,
                gst_analytics_tracking_mtd_get_mtd_type(), &related, &tracking)) {
            guint64 id;
            gst_analytics_tracking_mtd_get_info(
                reinterpret_cast<GstAnalyticsTrackingMtd *>(&tracking), &id, nullptr, nullptr,
                nullptr);
            json_object_set_int_member(object, "tracking_id", id);
        }
        auto *classifications = json_array_new();
        related = nullptr;
        GstAnalyticsMtd cls{};
        while (gst_analytics_relation_meta_get_direct_related(
            meta, item.id, GST_ANALYTICS_REL_TYPE_ANY, gst_analytics_cls_mtd_get_mtd_type(),
            &related, &cls)) {
            if (g_strcmp0(gst_analytics_mtd_get_semantic_tag(&cls), "class_descriptor") == 0)
                continue;
            auto *c = reinterpret_cast<GstAnalyticsClsMtd *>(&cls);
            for (unsigned i = 0; i < gst_analytics_cls_mtd_get_length(c); i++) {
                auto *value = json_object_new();
                auto *name = g_quark_to_string(gst_analytics_cls_mtd_get_quark(c, i));
                json_object_set_string_member(value, "label", name ? name : "");
                json_object_set_double_member(value, "confidence",
                                              gst_analytics_cls_mtd_get_level(c, i));
                json_array_add_object_element(classifications, value);
            }
        }
        json_object_set_array_member(object, "classifications", classifications);
        auto *keypoints = json_array_new();
        related = nullptr;
        GstAnalyticsMtd group{};
        while (gst_analytics_relation_meta_get_direct_related(
            meta, item.id, GST_ANALYTICS_REL_TYPE_ANY, gst_analytics_group_mtd_get_mtd_type(),
            &related, &group)) {
            gpointer member_cursor = nullptr;
            GstAnalyticsMtd member{};
            while (gst_analytics_group_mtd_iterate(
                reinterpret_cast<GstAnalyticsGroupMtd *>(&group), &member_cursor,
                gst_analytics_keypoint_mtd_get_mtd_type(), &member)) {
                gint px{}, py{};
                GstAnalyticsKeypointDimensions dimensions{};
                gfloat confidence{};
                auto *kp = reinterpret_cast<GstAnalyticsKeypointMtd *>(&member);
                gst_analytics_keypoint_mtd_get_position(kp, &px, &py, nullptr, &dimensions);
                gst_analytics_keypoint_mtd_get_confidence(kp, &confidence);
                auto *value = json_object_new();
                json_object_set_int_member(value, "x", px);
                json_object_set_int_member(value, "y", py);
                json_object_set_double_member(value, "confidence", confidence);
                json_array_add_object_element(keypoints, value);
            }
        }
        json_object_set_array_member(object, "keypoints", keypoints);
        json_array_add_object_element(objects, object);
    }
    json_object_set_array_member(root, "objects", objects);
    auto *full_classes = json_array_new();
    cursor = nullptr;
    while (meta && gst_analytics_relation_meta_iterate(
                       meta, &cursor, gst_analytics_cls_mtd_get_mtd_type(), &item)) {
        if (g_strcmp0(gst_analytics_mtd_get_semantic_tag(&item), "class_descriptor") == 0)
            continue;
        gpointer related = nullptr;
        GstAnalyticsMtd parent{};
        if (gst_analytics_relation_meta_get_direct_related(
                meta, item.id, GST_ANALYTICS_REL_TYPE_ANY, gst_analytics_od_mtd_get_mtd_type(),
                &related, &parent))
            continue;
        auto *cls = reinterpret_cast<GstAnalyticsClsMtd *>(&item);
        for (unsigned i = 0; i < gst_analytics_cls_mtd_get_length(cls); i++) {
            auto *value = json_object_new();
            auto *label = g_quark_to_string(gst_analytics_cls_mtd_get_quark(cls, i));
            json_object_set_string_member(value, "label", label ? label : "");
            json_object_set_double_member(value, "confidence",
                                          gst_analytics_cls_mtd_get_level(cls, i));
            json_array_add_object_element(full_classes, value);
        }
    }
    json_object_set_array_member(root, "classifications", full_classes);
    if (amd && amd->data && !(**amd->data).custom_json.empty()) {
        auto *parser = json_parser_new();
        if (json_parser_load_from_data(parser, (**amd->data).custom_json.c_str(), -1, nullptr))
            json_object_set_member(root, "custom", json_node_copy(json_parser_get_root(parser)));
        g_object_unref(parser);
    }
    auto *tensors = json_array_new();
    if (amd && amd->data)
        for (const auto &attachment : (**amd->data).attachments)
            for (const auto &tensor : attachment.result.tensors) {
                auto *value = json_object_new();
                json_object_set_string_member(value, "name", tensor.name.c_str());
                json_object_set_string_member(value, "storage", "gpu");
                const char *dtype = "unknown";
                switch (tensor.type) {
                case TensorType::Float32:
                    dtype = "float32";
                    break;
                case TensorType::Float16:
                    dtype = "float16";
                    break;
                case TensorType::Int32:
                    dtype = "int32";
                    break;
                case TensorType::Int64:
                    dtype = "int64";
                    break;
                case TensorType::UInt8:
                    dtype = "uint8";
                    break;
                }
                json_object_set_string_member(value, "dtype", dtype);
                json_object_set_int_member(value, "device", tensor.device);
                json_object_set_int_member(value, "batch_index", attachment.result.batch_index);
                auto *shape = json_array_new();
                for (auto d : tensor.shape)
                    json_array_add_int_element(shape, d);
                json_object_set_array_member(value, "shape", shape);
                json_array_add_object_element(tensors, value);
            }
    json_object_set_array_member(root, "tensors", tensors);
    auto *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, root);
    auto *generator = json_generator_new();
    json_generator_set_root(generator, node);
    auto *data = json_generator_to_data(generator, nullptr);
    std::string json = data;
    g_free(data);
    g_object_unref(generator);
    json_node_free(node);
    return json;
}
struct ResultPublisher::Impl {
    PublicationOptions options;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> queue;
    PublicationStatus state;
    std::atomic<bool> stop{false}, connected{false}, connecting{false};
    std::thread worker;
    MQTTAsync mqtt{};
    rd_kafka_t *kafka{};
    std::ofstream file;
    uint64_t pending{};
    explicit Impl(PublicationOptions o) : options(std::move(o)) {
        if (options.method != "none" && options.method != "file" && options.method != "mqtt" &&
            options.method != "kafka")
            throw std::runtime_error("Unknown publication method");
        if (!options.queue_limit || options.queue_limit > 65536 || !options.byte_limit ||
            options.byte_limit > 64 * 1024 * 1024)
            throw std::runtime_error("Invalid publication queue budget");
        if ((options.method == "file" && options.file_path.empty()) ||
            ((options.method == "mqtt" || options.method == "kafka") &&
             (options.address.empty() || options.topic.empty())))
            throw std::runtime_error("Missing publication destination");
#ifndef AMDVA_HAVE_MQTT
        if (options.method == "mqtt")
            throw std::runtime_error("MQTT publication is not built (AMDVA_WITH_MQTT=OFF)");
#endif
#ifndef AMDVA_HAVE_KAFKA
        if (options.method == "kafka")
            throw std::runtime_error("Kafka publication is not built (AMDVA_WITH_KAFKA=OFF)");
#endif
        if (options.method != "none")
            worker = std::thread([this] { run(); });
    }
    void failure(const std::string &message) {
        std::lock_guard<std::mutex> lock(mutex);
        ++state.errors;
        state.last_error = message;
    }
    void delivered(bool success) {
        std::lock_guard<std::mutex> lock(mutex);
        if (success)
            ++state.delivered;
        else
            ++state.dropped;
        if (pending)
            --pending;
        ready.notify_all();
    }
#ifdef AMDVA_HAVE_KAFKA
    static void kafka_delivery(rd_kafka_t *, const rd_kafka_message_t *message, void *opaque) {
        auto *self = static_cast<Impl *>(opaque);
        self->delivered(!message->err);
        if (message->err)
            self->failure(rd_kafka_err2str(message->err));
    }
#endif
    void initialize() {

#ifdef AMDVA_HAVE_MQTT
        if (options.method == "mqtt") {
            auto *uuid = g_uuid_string_random();
            std::string id = "amdva-" + std::string(uuid);
            g_free(uuid);
            if (MQTTAsync_create(&mqtt, options.address.c_str(), id.c_str(),
                                 MQTTCLIENT_PERSISTENCE_NONE, nullptr) != MQTTASYNC_SUCCESS)
                throw std::runtime_error("Cannot create MQTT client");
            if (MQTTAsync_setCallbacks(
                    mqtt, this,
                    [](void *context, char *) {
                        auto *self = static_cast<Impl *>(context);
                        self->connected = false;
                        self->failure("MQTT connection lost");
                    },
                    [](void *, char *topic, int, MQTTAsync_message *message) {
                        MQTTAsync_freeMessage(&message);
                        MQTTAsync_free(topic);
                        return 1;
                    },
                    nullptr) != MQTTASYNC_SUCCESS)
                throw std::runtime_error("Cannot register MQTT callbacks");
        }
#endif
#ifdef AMDVA_HAVE_KAFKA
        if (options.method == "kafka") {
            auto *conf = rd_kafka_conf_new();
            char error[512];
            auto set = [&](const char *key, const std::string &value) {
                if (rd_kafka_conf_set(conf, key, value.c_str(), error, sizeof(error)) !=
                    RD_KAFKA_CONF_OK) {
                    rd_kafka_conf_destroy(conf);
                    throw std::runtime_error(error);
                }
            };
            set("bootstrap.servers", options.address);
            set("queue.buffering.max.messages", std::to_string(options.queue_limit));
            set("queue.buffering.max.kbytes",
                std::to_string(std::max(size_t(1), options.byte_limit / 1024)));
            set("message.timeout.ms", "5000");
            set("socket.timeout.ms", "1000");
            rd_kafka_conf_set_opaque(conf, this);
            rd_kafka_conf_set_dr_msg_cb(conf, kafka_delivery);
            kafka = rd_kafka_new(RD_KAFKA_PRODUCER, conf, error, sizeof(error));
            if (!kafka) {
                rd_kafka_conf_destroy(conf);
                throw std::runtime_error(error);
            }
        }
#endif
    }
    void run() {
        try {
            initialize();
            auto retry = std::chrono::steady_clock::time_point{};
            while (!stop) {
#ifdef AMDVA_HAVE_MQTT
                if (mqtt && !connected && !connecting &&
                    std::chrono::steady_clock::now() >= retry) {
                    connecting = true;
                    retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    MQTTAsync_connectOptions c = MQTTAsync_connectOptions_initializer;
                    c.keepAliveInterval = 10;
                    c.connectTimeout = 1;
                    c.cleansession = 1;
                    c.context = this;
                    c.onSuccess = [](void *context, MQTTAsync_successData *) {
                        auto *self = static_cast<Impl *>(context);
                        self->connected = true;
                        self->connecting = false;
                    };
                    c.onFailure = [](void *context, MQTTAsync_failureData *) {
                        auto *self = static_cast<Impl *>(context);
                        self->connecting = false;
                        self->connected = false;
                        self->failure("MQTT connect failed");
                    };
                    if (MQTTAsync_connect(mqtt, &c) != MQTTASYNC_SUCCESS) {
                        connecting = false;
                        failure("MQTT connect rejected");
                    }
                }
#endif
                if (options.method == "file" && !file.is_open()) {
                    if (std::chrono::steady_clock::now() < retry) {
                        std::unique_lock<std::mutex> lock(mutex);
                        ready.wait_for(lock, std::chrono::milliseconds(50),
                                       [this] { return bool(stop); });
                        continue;
                    }
                    file.clear();
                    file.open(options.file_path, std::ios::app);
                    if (!file) {
                        failure("Cannot open result file");
                        retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                        continue;
                    }
                }
#ifdef AMDVA_HAVE_KAFKA
                if (kafka)
                    rd_kafka_poll(kafka, 0);
#endif
                std::string json;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait_for(lock, std::chrono::milliseconds(50), [this] {
                        return stop || (!queue.empty() && (!mqtt || connected));
                    });
                    if (stop)
                        break;
                    if (queue.empty() || (mqtt && !connected))
                        continue;
                    json = std::move(queue.front());
                    queue.pop_front();
                    state.queue_bytes -= json.size();
                }
                if (file.is_open()) {
                    file << json << '\n';
                    file.flush();
                    if (file)
                        delivered(true);
                    else {
                        delivered(false);
                        failure("Result file write failed");
                        file.close();
                        retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    }
                }
#ifdef AMDVA_HAVE_MQTT
                if (mqtt) {
                    MQTTAsync_message message = MQTTAsync_message_initializer;
                    message.payload = json.data();
                    message.payloadlen = int(json.size());
                    message.qos = 1;
                    MQTTAsync_responseOptions response = MQTTAsync_responseOptions_initializer;
                    if (MQTTAsync_sendMessage(mqtt, options.topic.c_str(), &message, &response) !=
                        MQTTASYNC_SUCCESS) {
                        delivered(false);
                        failure("MQTT send failed");
                        connected = false;
                        continue;
                    }
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (!MQTTAsync_isComplete(mqtt, response.token) && connected && !stop &&
                           std::chrono::steady_clock::now() < deadline)
                        (void)MQTTAsync_waitForCompletion(mqtt, response.token, 100);
                    const bool ok = MQTTAsync_isComplete(mqtt, response.token);
                    delivered(ok);
                    if (!ok) {
                        failure("MQTT delivery failed");
                        connected = false;
                        MQTTAsync_disconnectOptions d = MQTTAsync_disconnectOptions_initializer;
                        d.timeout = 100;
                        (void)MQTTAsync_disconnect(mqtt, &d);
                    }
                }
#endif
#ifdef AMDVA_HAVE_KAFKA
                if (kafka) {
                    if (rd_kafka_producev(kafka, RD_KAFKA_V_TOPIC(options.topic.c_str()),
                                          RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
                                          RD_KAFKA_V_VALUE(json.data(), json.size()),
                                          RD_KAFKA_V_END) != RD_KAFKA_RESP_ERR_NO_ERROR) {
                        delivered(false);
                        failure("Kafka produce queue rejected message");
                    }
                }
#endif
            }
        } catch (const std::exception &e) {
            failure(e.what());
            stop = true;
        }
#ifdef AMDVA_HAVE_MQTT
        if (mqtt)
            MQTTAsync_destroy(&mqtt);
#endif
#ifdef AMDVA_HAVE_KAFKA
        if (kafka) {
            rd_kafka_flush(kafka, 1000);
            rd_kafka_destroy(kafka);
            kafka = nullptr;
        }
#endif
        std::lock_guard<std::mutex> lock(mutex);
        state.dropped += queue.size();
        pending -= queue.size();
        queue.clear();
        state.queue_bytes = 0;
        ready.notify_all();
    }
    bool flush(unsigned timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return ready.wait_for(lock, std::chrono::milliseconds(timeout), [this] {
            return !pending || stop;
        }) && !pending;
    }
    ~Impl() {
        (void)flush(2000);
        stop = true;
        ready.notify_all();
        if (worker.joinable())
            worker.join();
    }
};
ResultPublisher::ResultPublisher(PublicationOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
ResultPublisher::~ResultPublisher() = default;
bool ResultPublisher::publish(std::string json) {
    auto &s = *impl_;
    if (s.options.method == "none")
        return false;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.stop || json.size() > 65536 || json.size() > s.options.byte_limit) {
        ++s.state.dropped;
        return false;
    }
    while (!s.queue.empty() && (s.queue.size() >= s.options.queue_limit ||
                                s.state.queue_bytes + json.size() > s.options.byte_limit)) {
        s.state.queue_bytes -= s.queue.front().size();
        s.queue.pop_front();
        ++s.state.dropped;
        --s.pending;
    }
    s.state.queue_bytes += json.size();
    s.queue.push_back(std::move(json));
    ++s.state.accepted;
    ++s.pending;
    s.ready.notify_one();
    return true;
}
PublicationStatus ResultPublisher::status() const {
    auto &s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    auto state = s.state;
    state.queued = s.queue.size();
    return state;
}
bool ResultPublisher::flush(unsigned timeout) { return impl_->flush(timeout); }
} // namespace amdva
