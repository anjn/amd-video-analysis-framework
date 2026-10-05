#include "controller.hpp"
#include "keyframe.hpp"
#include <fstream>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/va/gstva.h>
#include <gst/va/gstvadisplay_drm.h>
#include <gst/va/gstvautils.h>
#include <gst/video/video.h>
#include <iostream>
using namespace amdva::nvr;
static GstElement *pipeline(const std::string &text) {
    GError *error{};
    auto *p = gst_parse_launch(text.c_str(), &error);
    if (error || !p)
        throw std::runtime_error(error ? error->message : "Pipeline unavailable");
    return p;
}
int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: nvr-keyframe-tests H264|H265 FILE OUTPUT_PREFIX\n";
        return 1;
    }
    initialize_runtime();
    const int codec = std::string(argv[1]) == "H264" ? 1 : 2;
    const std::string parser = codec == 1 ? "h264parse" : "h265parse";
    auto *display = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    GstElement *reader{}, *decoder{};
    unsigned frames = 0, keys = 0, outputs = 0, parser_keys = 0;
    std::map<int, unsigned> types;
    try {
        reader = pipeline("filesrc name=file ! " + parser +
                          (codec == 1 ? " ! video/x-h264,stream-format=byte-stream,alignment=au"
                                      : " ! video/x-h265,stream-format=byte-stream,alignment=au") +
                          " ! appsink name=packets sync=false max-buffers=1");
        auto *file = gst_bin_get_by_name(GST_BIN(reader), "file");
        g_object_set(file, "location", argv[2], nullptr);
        gst_object_unref(file);
        auto *packets = gst_bin_get_by_name(GST_BIN(reader), "packets");
        gst_element_set_state(reader, GST_STATE_PLAYING);
        gst_element_get_state(reader, nullptr, nullptr, 3 * GST_SECOND);
        KeyframeCache cache;
        while (auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(packets), 3 * GST_SECOND)) {
            auto *buffer = gst_sample_get_buffer(sample);
            ++frames;
            parser_keys += !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
            if (cache.observe(buffer, codec)) {
                ++keys;
                types[cache.type()]++;
                // One AU + cached configuration; no dependent pictures.
                decoder = pipeline(
                    "appsrc name=input format=time ! " + parser +
                    (codec == 1 ? " ! vah264dec" : " ! vah265dec") +
                    " ! video/x-raw(memory:VAMemory),format=NV12 ! appsink name=output sync=false");
                auto *context = gst_context_new(GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR, TRUE);
                gst_context_set_va_display(context, display);
                gst_element_set_context(decoder, context);
                gst_context_unref(context);
                auto *input = gst_bin_get_by_name(GST_BIN(decoder), "input");
                auto *sink = gst_bin_get_by_name(GST_BIN(decoder), "output");
                auto *pad = gst_element_get_static_pad(sink, "sink");
                gst_pad_add_probe(
                    pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                    [](GstPad *, GstPadProbeInfo *info, gpointer) {
                        auto *query = GST_PAD_PROBE_INFO_QUERY(info);
                        if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION)
                            return GST_PAD_PROBE_OK;
                        gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
                        gst_query_add_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, nullptr);
                        return GST_PAD_PROBE_HANDLED;
                    },
                    nullptr, nullptr);
                gst_object_unref(pad);
                gst_app_src_set_caps(GST_APP_SRC(input), gst_sample_get_caps(sample));
                gst_element_set_state(decoder, GST_STATE_PLAYING);
                auto *one = cache.input(buffer);
                if (!one)
                    throw std::runtime_error("Missing configuration");
                {
                    GstMapInfo m{};
                    gst_buffer_map(one, &m, GST_MAP_READ);
                    std::ofstream out(std::string(argv[3]) +
                                          (keys == 1 ? ".au" : ".au" + std::to_string(keys)),
                                      std::ios::binary);
                    out.write(reinterpret_cast<const char *>(m.data), m.size);
                    gst_buffer_unmap(one, &m);
                }
                gst_app_src_push_buffer(GST_APP_SRC(input), one);
                gst_app_src_end_of_stream(GST_APP_SRC(input));
                gst_element_get_state(decoder, nullptr, nullptr, 2 * GST_SECOND);
                auto *decoded = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND);
                if (!decoded ||
                    gst_va_buffer_get_surface(gst_sample_get_buffer(decoded)) == VA_INVALID_SURFACE)
                    throw std::runtime_error("Single AU did not produce a VA frame");
                ++outputs;
                gst_sample_unref(decoded);
                if (auto *extra = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), GST_SECOND)) {
                    gst_sample_unref(extra);
                    throw std::runtime_error("Single AU produced multiple frames");
                }
                gst_element_set_state(decoder, GST_STATE_NULL);
                gst_object_unref(input);
                gst_object_unref(sink);
                gst_object_unref(decoder);
                decoder = nullptr;
            }
            gst_sample_unref(sample);
        }
        gst_object_unref(packets);
        if (!keys || outputs != keys || frames <= keys)
            throw std::runtime_error("Insufficient fixture coverage: frames=" +
                                     std::to_string(frames) + " keys=" + std::to_string(keys));
        // Truncated/missing-config IDR and non-IDR intra slices cannot be admitted.
        cache.reset();
        const std::vector<uint8_t> bad = codec == 1
                                             ? std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0x80}
                                             : std::vector<uint8_t>{0, 0, 0, 1, 0x26, 1, 0x80};
        auto *b = gst_buffer_new_allocate(nullptr, bad.size(), nullptr);
        gst_buffer_fill(b, 0, bad.data(), bad.size());
        if (cache.observe(b, codec))
            throw std::runtime_error("Missing parameters accepted");
        gst_buffer_unref(b);
        gst_element_set_state(reader, GST_STATE_NULL);
        gst_object_unref(reader);
        gst_object_unref(display);
        std::cout << "{\"nal_types\":{";
        bool first = true;
        for (const auto &t : types) {
            if (!first)
                std::cout << ",";
            first = false;
            std::cout << "\"" << t.first << "\":" << t.second;
        }
        std::cout << "},\"parser_keys\":" << parser_keys << ",\"passed\":true,\"frames\":" << frames
                  << ",\"keys\":" << keys << ",\"va_outputs\":" << outputs << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        if (decoder) {
            gst_element_set_state(decoder, GST_STATE_NULL);
            gst_object_unref(decoder);
        }
        if (reader) {
            gst_element_set_state(reader, GST_STATE_NULL);
            gst_object_unref(reader);
        }
        gst_object_unref(display);
        return 2;
    }
}
