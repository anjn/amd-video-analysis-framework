#include "amdva/streaming/config.hpp"
#include "../model_json.hpp"
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <unistd.h>
namespace amdva::streaming {
std::string validated_uri(std::string uri) {
    GError *error = nullptr;
    auto *parsed = g_uri_parse(uri.c_str(), G_URI_FLAGS_NONE, &error);
    if (!parsed) {
        g_clear_error(&error);
        throw std::runtime_error("Source URI must use RTSP with a host");
    }
    const auto *host = g_uri_get_host(parsed);
    const auto *scheme = g_uri_get_scheme(parsed);
    bool valid = host && *host && scheme &&
                 (!g_ascii_strcasecmp(scheme, "rtsp") || !g_ascii_strcasecmp(scheme, "rtsps"));
    if (valid) {
        auto *lower = g_ascii_strdown(scheme, -1);
        uri.replace(0, std::string(scheme).size(), lower);
        g_free(lower);
    }
    g_uri_unref(parsed);
    if (!valid)
        throw std::runtime_error("Source URI must use RTSP with a host");
    return uri;
}
Options load_config(const std::string &path) {
    Parser p(path);
    auto *root = p.object();
    Options o;
    const auto dir = std::filesystem::absolute(path).parent_path();
    auto resolve = [&](const char *key, const char *fallback = "") {
        std::string value =
            json_object_has_member(root, key) ? required_string(root, key) : fallback;
        return value.empty() ? value : (dir / value).lexically_normal().string();
    };
    auto number = [&](const char *key, double fallback) {
        if (!json_object_has_member(root, key))
            return fallback;
        auto *n = json_object_get_member(root, key);
        auto type = json_node_get_value_type(n);
        if (type != G_TYPE_INT64 && type != G_TYPE_DOUBLE)
            throw std::runtime_error(std::string("Expected numeric ") + key);
        auto value = json_node_get_double(n);
        if (!std::isfinite(value))
            throw std::runtime_error("Non-finite config number");
        return value;
    };
    o.model = resolve("model");
    o.single_model = resolve("single_model");
    o.model_info = resolve("model_info");
    o.recording_root = resolve("recording_root", "recordings");
    auto batch = number("batch_size", 4), seconds = number("segment_seconds", 60),
         interval = number("background_ai_interval_seconds", 5), quota = number("quota_gib", 100),
         reserve = number("reserve_gib", 1);
    if (batch != 1 && batch != 2 && batch != 4 && batch != 8 && batch != 16)
        throw std::runtime_error("Invalid batch size");
    if (seconds != std::floor(seconds) || seconds < 1 || seconds > 86400 ||
        interval != std::floor(interval) || interval < 1 || interval > 3600 || quota <= 0 ||
        quota > 1000000 || reserve < 0 || reserve > 1000000)
        throw std::runtime_error("Invalid storage, segment or background interval");
    o.batch = unsigned(batch);
    auto retention = number("retention_minutes", 60);
    if (retention < 0 || retention > 2147483647 || retention != std::floor(retention))
        throw std::runtime_error("Invalid retention_minutes");
    o.retention_minutes = unsigned(retention);
    o.segment_seconds = unsigned(seconds);
    o.background_ai_interval_seconds = unsigned(interval);
    o.quota = uint64_t(quota * (1ull << 30));
    o.reserve = uint64_t(reserve * (1ull << 30));
    if (json_object_has_member(root, "background_ai_enabled")) {
        if (json_node_get_value_type(json_object_get_member(root, "background_ai_enabled")) !=
            G_TYPE_BOOLEAN)
            throw std::runtime_error("Expected boolean background_ai_enabled");
        o.background_ai_enabled = json_object_get_boolean_member(root, "background_ai_enabled");
    }
    if (json_object_has_member(root, "tracking_enabled")) {
        if (json_node_get_value_type(json_object_get_member(root, "tracking_enabled")) !=
            G_TYPE_BOOLEAN)
            throw std::runtime_error("Expected boolean tracking_enabled");
        o.tracking_enabled = json_object_get_boolean_member(root, "tracking_enabled");
    }
    if (json_object_has_member(root, "telemetry")) {
        auto *t = required_object(root, "telemetry");
        if (json_object_has_member(t, "enabled")) {
            if (json_node_get_value_type(json_object_get_member(t, "enabled")) != G_TYPE_BOOLEAN)
                throw std::runtime_error("Expected boolean telemetry.enabled");
            o.telemetry.enabled = json_object_get_boolean_member(t, "enabled");
        }
        if (json_object_has_member(t, "stall_ms")) {
            auto *n = json_object_get_member(t, "stall_ms");
            if (json_node_get_value_type(n) != G_TYPE_INT64 &&
                json_node_get_value_type(n) != G_TYPE_DOUBLE)
                throw std::runtime_error("Expected numeric telemetry.stall_ms");
            const double v = json_node_get_double(n);
            if (!std::isfinite(v) || v != std::floor(v) || v < 1 || v > 3600000)
                throw std::runtime_error("Invalid telemetry.stall_ms");
            o.telemetry.stall_ms = unsigned(v);
        }
    }
    if (json_object_has_member(root, "search")) {
        auto *search = required_object(root, "search");
        if (json_object_has_member(search, "enabled")) {
            if (json_node_get_value_type(json_object_get_member(search, "enabled")) !=
                G_TYPE_BOOLEAN)
                throw std::runtime_error("Expected boolean search.enabled");
            o.search.enabled = json_object_get_boolean_member(search, "enabled");
        }
        for (const auto *key : {"library", "bundle"})
            if (json_object_has_member(search, key)) {
                std::string value = required_string(search, key);
                if (std::string(key) == "bundle")
                    o.search.bundle = (dir / value).lexically_normal().string();
                else
                    o.search.library = value.find('/') == std::string::npos
                                           ? value
                                           : (dir / value).lexically_normal().string();
            }
        for (auto field : {std::pair<const char *, unsigned *>{"image_size", &o.search.image_size},
                           {"device", &o.search.device},
                           {"interval_ms", &o.search.interval_ms},
                           {"queue_limit", &o.search.queue_limit}}) {
            if (!json_object_has_member(search, field.first))
                continue;
            auto *n = json_object_get_member(search, field.first);
            if (json_node_get_value_type(n) != G_TYPE_INT64)
                throw std::runtime_error("Search option must be an integer");
            auto v = json_node_get_int(n);
            if (v < 0 || v > 60000)
                throw std::runtime_error("Search option out of range");
            *field.second = unsigned(v);
        }
    }
    if (json_object_has_member(root, "search")) {
        auto *search = required_object(root, "search");
        if (json_object_has_member(search, "reid_model")) {
            std::string value = required_string(search, "reid_model");
            if (!value.empty())
                o.search.reid_model = (dir / value).lexically_normal().string();
        }
    }
    if (o.search.image_size < 32 || o.search.image_size > 1024 || o.search.device > 15 ||
        o.search.interval_ms < 100 || !o.search.queue_limit || o.search.queue_limit > 1024 ||
        (o.search.enabled && o.search.bundle.empty()))
        throw std::runtime_error("Invalid search configuration");
    if (o.search.enabled)
        o.background_ai_enabled = true;
    if (json_object_has_member(root, "results")) {
        auto *output = required_object(root, "results");
        if (json_object_has_member(output, "method"))
            o.results.method = required_string(output, "method");
        if (json_object_has_member(output, "address"))
            o.results.address = required_string(output, "address");
        if (json_object_has_member(output, "topic"))
            o.results.topic = required_string(output, "topic");
        if (json_object_has_member(output, "file_path")) {
            auto value = std::string(required_string(output, "file_path"));
            if (!value.empty())
                o.results.file_path = (dir / value).lexically_normal().string();
        }
        for (const auto *key : {"queue_limit", "byte_limit"})
            if (json_object_has_member(output, key)) {
                auto *n = json_object_get_member(output, key);
                if (json_node_get_value_type(n) != G_TYPE_INT64)
                    throw std::runtime_error("Publication budget must be an integer");
                auto value = json_node_get_int(n);
                if (value < 1 ||
                    value > (std::string(key) == "queue_limit" ? 65536 : 64 * 1024 * 1024))
                    throw std::runtime_error("Publication budget out of range");
                if (std::string(key) == "queue_limit")
                    o.results.queue_limit = unsigned(value);
                else
                    o.results.byte_limit = size_t(value);
            }
        if (o.results.method != "none" && o.results.method != "file" &&
            o.results.method != "mqtt" && o.results.method != "kafka")
            throw std::runtime_error("Invalid result publication method");
        if (o.results.method == "file" && o.results.file_path.empty())
            throw std::runtime_error("Result file path is required");
        if ((o.results.method == "mqtt" || o.results.method == "kafka") &&
            (o.results.address.empty() || o.results.topic.empty()))
            throw std::runtime_error("Result broker and topic are required");
    }
    if (o.model.empty() || o.model_info.empty() || (o.batch > 1 && o.single_model.empty()))
        throw std::runtime_error("model, model_info and single_model are required");
    std::set<std::string> ids;
    if (json_object_has_member(root, "sources")) {
        auto *sources = required_array(root, "sources");
        for (unsigned i = 0; i < json_array_get_length(sources); i++) {
            auto *node = json_array_get_element(sources, i);
            if (!JSON_NODE_HOLDS_OBJECT(node))
                throw std::runtime_error("Source must be an object");
            auto *source = json_node_get_object(node);
            Source s;
            if (json_object_has_member(source, "id"))
                s.id = required_string(source, "id");
            if (s.id.empty()) {
                auto *id = g_uuid_string_random();
                s.id = id;
                g_free(id);
            }
            if (s.id.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                    std::string::npos ||
                !ids.insert(s.id).second)
                throw std::runtime_error("Invalid or duplicate channel ID");
            s.name =
                json_object_has_member(source, "name") ? required_string(source, "name") : s.id;
            if (s.name.empty())
                s.name = s.id;
            s.uri = validated_uri(required_string(source, "uri"));
            if (json_object_has_member(source, "selected")) {
                if (json_node_get_value_type(json_object_get_member(source, "selected")) !=
                    G_TYPE_BOOLEAN)
                    throw std::runtime_error("selected must be boolean");
                s.selected = json_object_get_boolean_member(source, "selected");
            }
            o.sources.push_back(std::move(s));
        }
    }
    return o;
}
void save_config(const std::string &path, const Options &o) {
    auto *root = json_object_new();
    auto str = [&](const char *key, const std::string &value) {
        json_object_set_string_member(root, key, value.c_str());
    };
    str("model", o.model);
    str("single_model", o.single_model);
    str("model_info", o.model_info);
    str("recording_root", o.recording_root);
    json_object_set_boolean_member(root, "tracking_enabled", o.tracking_enabled);
    auto *timing = json_object_new();
    json_object_set_boolean_member(timing, "enabled", o.telemetry.enabled);
    json_object_set_int_member(timing, "stall_ms", o.telemetry.stall_ms);
    json_object_set_object_member(root, "telemetry", timing);
    json_object_set_int_member(root, "batch_size", o.batch);
    json_object_set_int_member(root, "segment_seconds", o.segment_seconds);
    json_object_set_int_member(root, "retention_minutes", o.retention_minutes);
    json_object_set_boolean_member(root, "background_ai_enabled", o.background_ai_enabled);
    json_object_set_int_member(root, "background_ai_interval_seconds",
                               o.background_ai_interval_seconds);
    json_object_set_double_member(root, "quota_gib", double(o.quota) / (1ull << 30));
    json_object_set_double_member(root, "reserve_gib", double(o.reserve) / (1ull << 30));
    auto *search = json_object_new();
    json_object_set_boolean_member(search, "enabled", o.search.enabled);
    json_object_set_string_member(search, "library", o.search.library.c_str());
    json_object_set_string_member(search, "bundle", o.search.bundle.c_str());
    json_object_set_string_member(search, "reid_model", o.search.reid_model.c_str());
    json_object_set_int_member(search, "device", o.search.device);
    json_object_set_int_member(search, "image_size", o.search.image_size);
    json_object_set_int_member(search, "interval_ms", o.search.interval_ms);
    json_object_set_int_member(search, "queue_limit", o.search.queue_limit);
    json_object_set_object_member(root, "search", search);
    auto *output = json_object_new();
    json_object_set_string_member(output, "method", o.results.method.c_str());
    json_object_set_string_member(output, "file_path", o.results.file_path.c_str());
    json_object_set_string_member(output, "address", o.results.address.c_str());
    json_object_set_string_member(output, "topic", o.results.topic.c_str());
    json_object_set_int_member(output, "queue_limit", o.results.queue_limit);
    json_object_set_int_member(output, "byte_limit", o.results.byte_limit);
    json_object_set_object_member(root, "results", output);
    auto *sources = json_array_new();
    for (const auto &s : o.sources) {
        auto *source = json_object_new();
        json_object_set_string_member(source, "id", s.id.c_str());
        json_object_set_string_member(source, "name", s.name.c_str());
        json_object_set_string_member(source, "uri", s.uri.c_str());
        json_object_set_boolean_member(source, "selected", s.selected);
        json_array_add_object_element(sources, source);
    }
    json_object_set_array_member(root, "sources", sources);
    auto *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, root);
    auto *generator = json_generator_new();
    json_generator_set_root(generator, node);
    json_generator_set_pretty(generator, TRUE);
    gsize size;
    auto *data = json_generator_to_data(generator, &size);
    g_object_unref(generator);
    json_node_free(node);
    std::string temporary = path + ".XXXXXX";
    int fd = g_mkstemp_full(temporary.data(), O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        g_free(data);
        throw std::runtime_error("Cannot create temporary config");
    }
    bool ok = true;
    size_t offset = 0;
    while (offset < size) {
        auto n = write(fd, data + offset, size - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        offset += size_t(n);
    }
    g_free(data);
    if (fsync(fd))
        ok = false;
    if (close(fd))
        ok = false;
    if (ok && rename(temporary.c_str(), path.c_str()) == 0) {
        auto dir = std::filesystem::absolute(path).parent_path();
        int directory = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            (void)fsync(directory);
            close(directory);
        }
        return;
    }
    unlink(temporary.c_str());
    throw std::runtime_error("Cannot commit config");
}
} // namespace amdva::streaming
