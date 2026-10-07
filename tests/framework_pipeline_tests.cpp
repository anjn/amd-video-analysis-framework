#include "amdva/gst_telemetry.hpp"
#include "amdva/publishing.hpp"
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <gst/allocators/gstdmabuf.h>
#include <gst/analytics/analytics.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
using namespace amdva;
static void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
struct Check {
    std::shared_ptr<Telemetry> timing = std::make_shared<Telemetry>("pipeline");
    Check() { timing->begin_generation(1); }
    std::mutex mutex;
    std::map<GstMemory *, std::set<GstClockTime>> original;
    bool failed{}, audit_started{};
    unsigned checked{};
    void (*audit_start)() = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
    ~Check() {
        for (const auto &[memory, pts] : original)
            gst_memory_unref(memory);
    }
    static GstPadProbeReturn origin(GstPad *, GstPadProbeInfo *info, gpointer opaque) {
        auto &self = *static_cast<Check *>(opaque);
        auto *buffer = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
        GST_PAD_PROBE_INFO_DATA(info) = buffer;
        TraceToken token{self.timing, self.timing->frame(Telemetry::monotonic_ns())};
        token.mark(TimingStage::DecodeOutput);
        set_frame_trace(buffer, token);
        auto *memory = gst_buffer_peek_memory(buffer, 0);
        std::lock_guard<std::mutex> lock(self.mutex);
        if (self.audit_start && self.checked && !self.audit_started) {
            self.audit_start();
            self.audit_started = true;
        }
        if (!self.original.count(memory))
            gst_memory_ref(memory);
        self.original[memory].insert(GST_BUFFER_PTS(buffer));
        return GST_PAD_PROBE_OK;
    }
    static GstPadProbeReturn result(GstPad *, GstPadProbeInfo *info, gpointer opaque) {
        auto &self = *static_cast<Check *>(opaque);
        auto *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        std::lock_guard<std::mutex> lock(self.mutex);
        auto it = self.original.find(gst_buffer_peek_memory(buffer, 0));
        if (!frame_trace(buffer).telemetry)
            self.failed = true;
        if (it == self.original.end() || !it->second.count(GST_BUFFER_PTS(buffer)))
            self.failed = true;
        ++self.checked;
        return GST_PAD_PROBE_OK;
    }
};
static unsigned run(const std::string &description, const std::string &kind) {
    Check check;
    GError *error = nullptr;
    auto *pipeline = gst_parse_launch(description.c_str(), &error);
    if (error) {
        std::string message = error->message;
        g_clear_error(&error);
        if (pipeline)
            gst_object_unref(pipeline);
        throw std::runtime_error(message);
    }
    std::vector<GstElement *> sinks;
    std::vector<GstSample *> retained;
    auto finish = [&] {
        if (auto stop = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_stop")))
            stop();
        gst_element_set_state(pipeline, GST_STATE_NULL);
        for (auto *sink : sinks)
            gst_object_unref(sink);
        gst_object_unref(pipeline);
    };
    try {
        for (const auto *name : {"origin", "origin0", "origin1", "checked", "checked0", "checked1"})
            if (auto *element = gst_bin_get_by_name(GST_BIN(pipeline), name)) {
                auto *pad = gst_element_get_static_pad(element, "src");
                gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                                  std::string(name).rfind("origin", 0) == 0 ? Check::origin
                                                                            : Check::result,
                                  &check, nullptr);
                gst_object_unref(pad);
                gst_object_unref(element);
            }
        for (const auto *name : {"output", "output0", "output1"})
            if (auto *element = gst_bin_get_by_name(GST_BIN(pipeline), name))
                sinks.push_back(element);
        require(!sinks.empty(), "No pipeline test sink");
        require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
                "Pipeline start failed");
        unsigned frames = 0;
        std::set<std::string> sources;
        for (auto *sink : sinks) {
            unsigned stream_frames = 0;
            while (auto *sample =
                       gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND)) {
                auto *buffer = gst_sample_get_buffer(sample);
                require(frame_trace(buffer).telemetry == check.timing,
                        "Timing metadata lost through AMD/DLS pipeline");
                auto *amd = get_analysis_meta(buffer);
                require(amd && amd->data && !((**amd->data).attachments.empty()),
                        "AMD metadata lost");
                auto &data = **amd->data;
                require(!data.identity.source.empty(), "Source identity missing");
                sources.insert(data.identity.source);
                require(data.identity.pts == GST_BUFFER_PTS(buffer), "Metadata PTS changed");
                if (kind == "classify") {
                    require(data.attachments.size() >= 2, "ROI classification attachment missing");
                    bool classified = false;
                    for (const auto &attachment : data.attachments)
                        if (attachment.parent_id >= 0 && !attachment.result.classifications.empty())
                            classified = true;
                    require(classified, "ROI classification relation missing");
                    require(data.custom_json.find("objects_seen") != std::string::npos,
                            "Python custom metadata missing");
                }
                if (kind == "semantic")
                    require(data.attachments.back().result.tensors[0].name == "semantic-mask",
                            "Semantic mask missing");
                if (kind == "pose")
                    require(!data.attachments.back().result.objects.empty() &&
                                data.attachments.back().result.objects[0].keypoints.size() == 17,
                            "Pose result missing");
                if (kind == "aggregate") {
                    require(data.attachments.size() == 1, "Aggregate duplicated AMD attachments");
                    require(!data.attachments[0].result.tensors.empty(),
                            "Aggregate lost GPU mask ownership");
                }
                if (kind != "mux")
                    require(gst_is_dmabuf_memory(gst_buffer_peek_memory(buffer, 0)),
                            "Watermark output is not DMA-BUF");
                if (retained.empty())
                    retained.push_back(gst_sample_ref(sample));
                ++frames;
                ++stream_frames;
                gst_sample_unref(sample);
            }
            require(stream_frames > 0, "A mux stream produced no output");
        }
        if (kind == "mux")
            require(sources.size() == 2, "Mux source identities were mixed");
        auto *bus = gst_element_get_bus(pipeline);
        auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (message) {
            GError *why = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &why, &debug);
            std::string text = why ? why->message : "Pipeline error";
            if (debug)
                text += " : " + std::string(debug);
            g_clear_error(&why);
            g_free(debug);
            gst_message_unref(message);
            throw std::runtime_error(text);
        }
        require(frames > 0 && check.checked > 0 && !check.failed,
                "Input memory or PTS changed before overlay");
        if (auto errors = reinterpret_cast<unsigned long long (*)()>(
                dlsym(RTLD_DEFAULT, "amdva_audit_errors"))) {
            require(check.audit_started && errors() == 0, "Runtime pixel/copy audit failed");
        }
        finish();
        // GPU attachments and exported overlay memory must outlive the pipeline.
        for (auto *sample : retained) {
            auto *meta = get_analysis_meta(gst_sample_get_buffer(sample));
            require(meta && meta->data, "Output metadata lifetime");
            gst_sample_unref(sample);
        }
        std::cout << kind << ": " << frames << " frames passed\n";
        return frames;
    } catch (...) {
        finish();
        for (auto *sample : retained)
            gst_sample_unref(sample);
        throw;
    }
}
static void flush_replacement(const std::string &first, const std::string &second,
                              const std::string &detector) {
    struct Decoded {
        GstElement *pipeline{};
        GstSample *sample{};
        explicit Decoded(const std::string &path) {
            pipeline =
                gst_parse_launch("filesrc name=input ! h264parse ! vah264dec ! "
                                 "video/x-raw(memory:VAMemory),format=NV12 ! appsink name=output "
                                 "sync=false async=false max-buffers=1 drop=true",
                                 nullptr);
            auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
            g_object_set(source, "location", path.c_str(), nullptr);
            gst_object_unref(source);
            auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
            sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
            gst_object_unref(sink);
            require(sample, "Replacement decode fixture");
        }
        ~Decoded() {
            gst_sample_unref(sample);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
    } a(first), b(second);
    auto *pipeline = gst_parse_launch(("appsrc name=input format=time ! " + detector +
                                       " ! appsink name=output sync=false async=false")
                                          .c_str(),
                                      nullptr);
    require(pipeline, "Replacement pipeline parse");
    auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
    auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
    auto finish = [&] {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(source);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
    };
    try {
        gst_app_src_set_caps(GST_APP_SRC(source), gst_sample_get_caps(a.sample));
        require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
                "Replacement pipeline start");
        auto push = [&](GstSample *sample, GstClockTime pts) {
            auto *buffer = gst_buffer_copy(gst_sample_get_buffer(sample));
            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
            require(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK,
                    "Replacement input");
        };
        push(a.sample, 0);
        auto *output = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
        require(output, "Initial inference output");
        auto generation =
            (**get_analysis_meta(gst_sample_get_buffer(output))->data).identity.generation;
        gst_sample_unref(output);
        auto *pad = gst_element_get_static_pad(source, "src");
        require(gst_pad_push_event(pad, gst_event_new_flush_start()), "Flush start");
        require(gst_pad_push_event(pad, gst_event_new_flush_stop(TRUE)), "Flush stop");
        require(gst_pad_push_event(pad, gst_event_new_stream_start("replacement")),
                "Replacement stream-start");
        gst_app_src_set_caps(GST_APP_SRC(source), gst_sample_get_caps(b.sample));
        require(gst_pad_push_event(pad, gst_event_new_caps(gst_sample_get_caps(b.sample))),
                "Replacement caps");
        GstSegment segment;
        gst_segment_init(&segment, GST_FORMAT_TIME);
        require(gst_pad_push_event(pad, gst_event_new_segment(&segment)), "Replacement segment");
        gst_object_unref(pad);
        push(b.sample, 2 * GST_SECOND);
        output = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
        require(output, "Replacement inference output");
        auto *buffer = gst_sample_get_buffer(output);
        auto identity = (**get_analysis_meta(buffer)->data).identity;
        require(identity.source == "replacement" && identity.generation > generation &&
                    identity.sequence == 0 && identity.pts == 2 * GST_SECOND,
                "Replacement generation/source/PTS isolation");
        require(gst_buffer_peek_memory(buffer, 0) ==
                    gst_buffer_peek_memory(gst_sample_get_buffer(b.sample), 0),
                "Replacement image memory changed");
        gst_sample_unref(output);
        finish();
        std::cout << "flush/caps/VA-display replacement passed\n";
    } catch (...) {
        finish();
        throw;
    }
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc != 4) {
        std::cerr
            << "Usage: framework-pipeline-tests INPUT.h264 MODEL_DIRECTORY DETECTION_DIRECTORY\n";
        return 1;
    }
    try {
        std::filesystem::path models = argv[2], detect = argv[3];
        auto quoted = [](const std::filesystem::path &p) {
            auto *value = g_strescape(p.c_str(), nullptr);
            std::string result = "\"" + std::string(value) + "\"";
            g_free(value);
            return result;
        };
        auto source = "filesrc location=" + quoted(argv[1]) +
                      " ! h264parse ! vah264dec ! video/x-raw(memory:VAMemory),format=NV12 ! "
                      "identity name=origin";
        auto element = [&](const std::string &factory, const std::filesystem::path &dir,
                           const std::string &name) {
            return factory + " model=" + quoted(dir / (name + ".mxr")) +
                   " model-info=" + quoted(dir / (name + ".json"));
        };
        auto detector = "amddetect model=" + quoted(detect / "yolov8n-b1.mxr") +
                        " model-info=" + quoted(detect / "model-info.json");
        flush_replacement(argv[1], (models / "small.h264").string(), detector);
        auto callback = models / "callback.py";
        std::ofstream(callback) << "from dataclasses import dataclass\n@dataclass\nclass Counts:\n "
                                   "   value: int\ndef process(frame):\n    return "
                                   "{'objects_seen': Counts(len(frame['objects'])).value}\n";
        run(source + " ! amdpython module=" + quoted(callback) + " ! " + detector +
                " ! gvatrack tracking-type=short-term-imageless ! " +
                element("amdclassify", models, "resnet18") +
                " ! amdpython module=" + quoted(callback) +
                " ! identity name=checked ! amdwatermark ! appsink name=output sync=false "
                "max-buffers=4",
            "classify");
        run(source + " ! " + element("amddetect", models, "yolov8n-pose") +
                " ! identity name=checked ! amdwatermark ! appsink name=output sync=false "
                "max-buffers=4",
            "pose");
        run(source + " ! " + element("amdinference", models, "segformer-b0") +
                " ! identity name=checked ! amdwatermark ! appsink name=output sync=false "
                "max-buffers=4",
            "semantic");
        run(source + " ! tee name=branch branch. ! queue ! aggregate.sink_0 branch. ! queue ! " +
                element("amddetect", models, "yolov8n-seg") +
                " ! aggregate.sink_1 gvametaaggregate name=aggregate ! identity name=checked ! "
                "amdwatermark ! appsink name=output sync=false max-buffers=4",
            "aggregate");
        auto source0 = "filesrc location=" + quoted(argv[1]) +
                       " ! h264parse ! vah264dec ! video/x-raw(memory:VAMemory),format=NV12 ! "
                       "identity name=origin0 ! mux.sink_0 ";
        auto source1 = "filesrc location=" + quoted(models / "small.h264") +
                       " ! h264parse ! vah264dec ! video/x-raw(memory:VAMemory),format=NV12 ! "
                       "identity name=origin1 ! mux.sink_1 ";
        run(source0 + source1 +
                "gvastreammux name=mux output-mode=container ! amddetect batch-size=4 model=" +
                quoted(detect / "yolov8n-b4.mxr") +
                " model-info=" + quoted(detect / "model-info.json") +
                " ! gvastreamdemux name=demux demux.src_0 ! queue ! identity name=checked0 ! "
                "appsink name=output0 sync=false async=false max-buffers=64 demux.src_1 ! queue ! "
                "identity name=checked1 ! appsink name=output1 sync=false async=false "
                "max-buffers=64",
            "mux");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
