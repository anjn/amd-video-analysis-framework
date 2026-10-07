#pragma once
#include <json-glib/json-glib.h>
#include <stdexcept>
#include <string>
namespace {
struct Parser {
    JsonParser *parser{json_parser_new()};
    explicit Parser(const std::string &path) {
        GError *e = nullptr;
        if (!json_parser_load_from_file(parser, path.c_str(), &e)) {
            const std::string message = e ? e->message : "JSON load failed";
            g_clear_error(&e);
            g_object_unref(parser);
            parser = nullptr;
            throw std::runtime_error(message);
        }
        if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
            g_object_unref(parser);
            parser = nullptr;
            throw std::runtime_error("JSON root must be an object");
        }
    }
    ~Parser() {
        if (parser)
            g_object_unref(parser);
    }
    JsonObject *object() const { return json_node_get_object(json_parser_get_root(parser)); }
};
static const char *required_string(JsonObject *o, const char *key) {
    if (!json_object_has_member(o, key) ||
        json_node_get_value_type(json_object_get_member(o, key)) != G_TYPE_STRING)
        throw std::runtime_error(std::string("Missing string: ") + key);
    return json_object_get_string_member(o, key);
}
static JsonObject *required_object(JsonObject *o, const char *key) {
    if (!json_object_has_member(o, key) || !JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, key)))
        throw std::runtime_error(std::string("Missing object: ") + key);
    return json_object_get_object_member(o, key);
}
static JsonArray *required_array(JsonObject *o, const char *key) {
    if (!json_object_has_member(o, key) || !JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, key)))
        throw std::runtime_error(std::string("Missing array: ") + key);
    return json_object_get_array_member(o, key);
}
static std::string sha256_file(const char *path) {
    gchar *data{};
    gsize size{};
    GError *e = nullptr;
    if (!g_file_get_contents(path, &data, &size, &e)) {
        std::string error = e ? e->message : "Read failed";
        g_clear_error(&e);
        throw std::runtime_error(error);
    }
    gchar *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                              reinterpret_cast<const guchar *>(data), size);
    std::string result = hash;
    g_free(hash);
    g_free(data);
    return result;
}
} // namespace
