#include "amdva/overlay.hpp"
#include "amdva/publishing.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <gst/analytics/analytics.h>
#include <iostream>
#include <json-glib/json-glib.h>
#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace amdva;
using namespace std::chrono_literals;
static void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
template <class F> static void eventually(F f, const char *why) {
    for (int i = 0; i < 100; i++) {
        if (f())
            return;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error(why);
}
// Local MQTT 3.1.1 protocol fixture: CONNECT, QoS1 PUBLISH, PING, disconnect.
// No external broker or network service is used by this test.
class Broker {
    int socket_;
    std::atomic<bool> stop{false};
    std::thread worker;
    bool read(int fd, void *data, size_t size) {
        auto *p = static_cast<unsigned char *>(data);
        while (size && !stop) {
            pollfd f{fd, POLLIN, 0};
            if (poll(&f, 1, 100) <= 0)
                continue;
            auto n = recv(fd, p, size, 0);
            if (n <= 0)
                return false;
            p += n;
            size -= n;
        }
        return !size;
    }
    void run() {
        while (!stop) {
            pollfd f{socket_, POLLIN, 0};
            if (poll(&f, 1, 100) <= 0)
                continue;
            int client = accept(socket_, nullptr, nullptr);
            if (client < 0)
                continue;
            while (!stop && online) {
                unsigned char type{}, byte{};
                if (!read(client, &type, 1))
                    break;
                size_t size = 0, mult = 1;
                do {
                    if (!read(client, &byte, 1)) {
                        size = 65537;
                        break;
                    }
                    size += (byte & 127) * mult;
                    mult *= 128;
                } while (byte & 128 && mult <= 2097152);
                if (size > 65536)
                    break;
                std::vector<unsigned char> body(size);
                if (!read(client, body.data(), size))
                    break;
                if ((type >> 4) == 1) {
                    unsigned char ack[] = {0x20, 2, 0, 0};
                    send(client, ack, sizeof(ack), MSG_NOSIGNAL);
                } else if ((type >> 4) == 3) {
                    require(body.size() >= 4, "MQTT fixture packet");
                    size_t topic = (size_t(body[0]) << 8) | body[1];
                    if (topic + 4 > body.size())
                        break;
                    unsigned char ack[] = {0x40, 2, body[topic + 2], body[topic + 3]};
                    send(client, ack, sizeof(ack), MSG_NOSIGNAL);
                    ++received;
                } else if ((type >> 4) == 12) {
                    unsigned char ack[] = {0xd0, 0};
                    send(client, ack, sizeof(ack), MSG_NOSIGNAL);
                } else if ((type >> 4) == 14)
                    break;
            }
            shutdown(client, SHUT_RDWR);
            close(client);
        }
    }

  public:
    std::atomic<bool> online{true};
    std::atomic<unsigned> received{0};
    unsigned port{};
    Broker() {
        socket_ = socket(AF_INET, SOCK_STREAM, 0);
        require(socket_ >= 0, "broker socket");
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(socket_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0, "broker bind");
        socklen_t n = sizeof(a);
        getsockname(socket_, reinterpret_cast<sockaddr *>(&a), &n);
        port = ntohs(a.sin_port);
        require(listen(socket_, 4) == 0, "broker listen");
        worker = std::thread([this] { run(); });
    }
    ~Broker() {
        stop = true;
        shutdown(socket_, SHUT_RDWR);
        if (worker.joinable())
            worker.join();
        close(socket_);
    }
};
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    try {
        auto *buffer = gst_buffer_new();
        ModelInfo model;
        model.classes = 1;
        model.labels = {"person"};
        AnalysisResult result;
        result.classifications = {{0, .75f}};
        Instance object;
        object.box = {10, 20, 30, 40, .9f, 0};
        object.tracking_id = 42;
        object.keypoints = {{12, 23, .8f}};
        object.classifications = {{0, .5f}};
        result.objects.push_back(object);
        attach_analysis(buffer, result, model, ImageTransform::make(100, 100, 100, 100),
                        {"camera", 2, 3, 123});
        auto overlay = overlay_from_buffer(buffer, 100, 100);
        require(overlay.labels.size() == 1 && overlay.labels[0] == "person" &&
                    overlay.objects[0].label == "person person",
                "Classification overlay labels");
        auto json = analysis_json(buffer);
        auto *parser = json_parser_new();
        require(json_parser_load_from_data(parser, json.data(), json.size(), nullptr),
                "Invalid JSON");
        auto *root = json_node_get_object(json_parser_get_root(parser));
        require(json_object_get_int_member(root, "generation") == 2, "JSON generation");
        require(json_array_get_length(json_object_get_array_member(root, "classifications")) == 1,
                "Full-frame classifications");
        auto *entry =
            json_array_get_object_element(json_object_get_array_member(root, "objects"), 0);
        require(json_object_get_int_member(entry, "tracking_id") == 42, "JSON tracking ID");
        require(json_array_get_length(json_object_get_array_member(entry, "classifications")) == 1,
                "ROI classifications");
        require(json_array_get_length(json_object_get_array_member(entry, "keypoints")) == 1,
                "JSON keypoints");
        require(json_object_get_int_member(json_array_get_object_element(
                                               json_object_get_array_member(entry, "keypoints"), 0),
                                           "x") == 12,
                "Keypoint coordinates");
        g_object_unref(parser);
        gst_buffer_unref(buffer);
        auto directory = std::filesystem::temp_directory_path() /
                         std::string("amdva-publish-" + std::to_string(getpid()));
        std::filesystem::create_directory(directory);
        {
            PublicationOptions o;
            o.method = "file";
            o.file_path = (directory / "delayed" / "events.jsonl").string();
            o.queue_limit = 4;
            o.byte_limit = 1024;
            ResultPublisher p(o);
            eventually([&] { return p.status().errors > 0; },
                       "Missing file destination not reported");
            for (int i = 0; i < 100; i++)
                p.publish(json);
            auto status = p.status();
            require(status.queued <= 4 && status.queue_bytes <= 1024 && status.dropped > 0,
                    "File queue is unbounded");
            std::filesystem::create_directory(directory / "delayed");
            require(p.flush(4000), "File publication did not recover");
            require(p.status().delivered > 0, "File records not delivered");
        }
        {
            Broker broker;
            PublicationOptions o;
            o.method = "mqtt";
            o.address = "tcp://127.0.0.1:" + std::to_string(broker.port);
            o.queue_limit = 4;
            ResultPublisher p(o);
            require(p.publish(json) && p.flush(6000), "MQTT delivery");
            require(broker.received == 1, "MQTT QoS1 acknowledgement");
            broker.online = false; // Close the connection when the next publication arrives.
            p.publish(json);
            eventually([&] { return p.status().errors > 0; },
                       "MQTT connection failure not observed");
            for (int i = 0; i < 100; i++)
                p.publish(json);
            require(p.status().queued <= 4, "MQTT queue unbounded");
            broker.online = true;
            p.publish(json);
            require(p.flush(12000), "MQTT reconnection");
            require(p.status().delivered >= 2, "MQTT recovery delivery");
        }
        {
            char error[512];
            auto *seed = rd_kafka_new(RD_KAFKA_PRODUCER, rd_kafka_conf_new(), error, sizeof(error));
            require(seed, "Kafka fixture producer");
            auto *cluster = rd_kafka_mock_cluster_new(seed, 1);
            require(cluster, "Kafka mock cluster");
            require(rd_kafka_mock_topic_create(cluster, "analytics", 1, 1) == 0,
                    "Kafka mock topic");
            {
                PublicationOptions o;
                o.method = "kafka";
                o.address = rd_kafka_mock_cluster_bootstraps(cluster);
                o.topic = "analytics";
                o.queue_limit = 4;
                ResultPublisher p(o);
                p.publish(json);
                require(p.flush(10000) && p.status().delivered == 1, "Kafka delivery");
                rd_kafka_mock_broker_set_down(cluster, 1);
                for (int i = 0; i < 100; i++)
                    p.publish(json);
                require(p.status().queued <= 4, "Kafka queue unbounded");
                rd_kafka_mock_broker_set_up(cluster, 1);
                require(p.flush(10000), "Kafka recovery");
                p.publish(json);
                require(p.flush(10000) && p.status().delivered >= 2, "Kafka recovered publication");
            }
            rd_kafka_mock_cluster_destroy(cluster);
            rd_kafka_destroy(seed);
        }
        std::filesystem::remove_all(directory);
        std::cout
            << "Metadata JSON, bounded queues, file/MQTT/Kafka delivery and recovery passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
