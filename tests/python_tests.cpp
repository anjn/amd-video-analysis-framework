#include "amdva/tensor_meta.hpp"
#include <filesystem>
#include <fstream>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <iostream>
#include <unistd.h>
using namespace amdva;
static void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
static void run(const std::string &path, const char *function, unsigned expected,
                bool expect_error = false) {
    GError *error = nullptr;
    auto *pipeline = gst_parse_launch("appsrc name=input format=time ! amdpython name=hook ! "
                                      "appsink name=output sync=false async=false",
                                      &error);
    require(pipeline && !error, "Python test pipeline parse");
    auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
    auto *hook = gst_bin_get_by_name(GST_BIN(pipeline), "hook");
    g_object_set(hook, "module", path.c_str(), "function", function, nullptr);
    gst_object_unref(hook);
    auto *caps = gst_caps_new_empty_simple("application/octet-stream");
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    bool failed = gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE;
    std::vector<GstMemory *> payloads;
    unsigned received = 0;
    if (!failed) {
        for (unsigned i = 0; i < 3; i++) {
            auto *buffer = gst_buffer_new_allocate(nullptr, 32, nullptr);
            GST_BUFFER_PTS(buffer) = i * GST_SECOND;
            auto data = std::make_shared<AnalysisData>();
            data->identity = {"camera", 1, i, GST_BUFFER_PTS(buffer)};
            add_analysis_meta(buffer, std::move(data));
            payloads.push_back(gst_memory_ref(gst_buffer_peek_memory(buffer, 0)));
            if (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) != GST_FLOW_OK)
                break;
        }
        gst_app_src_end_of_stream(GST_APP_SRC(source));
        while (auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND)) {
            auto *buffer = gst_sample_get_buffer(sample);
            auto *meta = get_analysis_meta(buffer);
            auto sequence = (**meta->data).identity.sequence;
            require(sequence < payloads.size() &&
                        gst_buffer_peek_memory(buffer, 0) == payloads[sequence],
                    "Python touched image payload");
            require(GST_BUFFER_PTS(buffer) == sequence * GST_SECOND, "Python changed PTS");
            if (std::string(function) == "custom")
                require((**meta->data).custom_json.find("module_count") != std::string::npos,
                        "Python custom metadata missing");
            ++received;
            gst_sample_unref(sample);
        }
        auto *bus = gst_element_get_bus(pipeline);
        auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        failed = message != nullptr;
        if (message)
            gst_message_unref(message);
        gst_object_unref(bus);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    for (auto *memory : payloads)
        gst_memory_unref(memory);
    require(failed == expect_error, "Python callback error contract");
    require(received == expected, "Python callback pass/drop contract");
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    auto path = std::filesystem::temp_directory_path() /
                std::string("amdva-python-" + std::to_string(getpid()) + ".py");
    try {
        std::ofstream(path) << R"(from dataclasses import dataclass
import sys
@dataclass
class Count:
    value: int
def keep(frame):
    assert frame['source'] == 'camera'
    return None
def drop(frame):
    return False
def custom(frame):
    count = Count(len([name for name in sys.modules if name.startswith('amdva_callback_')]))
    assert count.value == 1, 'previous callback module leaked'
    return {'module_count': count.value, 'sequence': frame['sequence']}
def broken(frame):
    raise RuntimeError('intentional callback error')
def wrong_type(frame):
    return 7
def nonfinite(frame):
    return {'value': float('nan')}
def oversized(frame):
    return {'text': 'a' * 65536}
)";
        run(path, "keep", 3);
        run(path, "drop", 0);
        run(path, "custom", 3);
        run(path, "custom", 3);
        run(path, "broken", 0, true);
        run(path, "wrong_type", 0, true);
        run(path, "oversized", 0, true);
        run(path, "nonfinite", 0, true);
        run(path, "missing", 0, true);
        std::filesystem::remove(path);
        std::cout << "Python metadata pass/drop/custom, dataclass import, module lifetime, "
                     "exceptions and size/type rejection passed\n";
    } catch (const std::exception &e) {
        std::filesystem::remove(path);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
