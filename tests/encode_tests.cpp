// amdencode contract: VAMemory and DMA-BUF input, H.264/H.265/AV1 output, BT.709
// signalling and frame accounting. Usage: encode-tests [VIDEO.h264 DETECT_DIR]
// With the optional arguments the annotated decode -> detect -> watermark ->
// encode path (BGRA DMA-BUF input) is also checked.
#include <filesystem>
#include <gst/app/gstappsink.h>
#include <gst/codecparsers/gsth264parser.h>
#include <gst/gst.h>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
struct Result {
    unsigned frames{};
    std::string media_type;
    GstBuffer *first{};
    ~Result() {
        if (first)
            gst_buffer_unref(first);
    }
};
static void run(const std::string &description, Result &result) {
    GError *error{};
    auto *pipeline = gst_parse_launch((description + " ! appsink name=out sync=false").c_str(), &error);
    require(pipeline && !error, error ? error->message : "Cannot parse pipeline");
    auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "out");
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
            "Pipeline did not start: " + description);
    for (;;) {
        auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 20 * GST_SECOND);
        if (!sample)
            break;
        if (!result.frames) {
            result.media_type = gst_structure_get_name(
                gst_caps_get_structure(gst_sample_get_caps(sample), 0));
            result.first = gst_buffer_ref(gst_sample_get_buffer(sample));
        }
        ++result.frames;
        gst_sample_unref(sample);
    }
    auto *bus = gst_element_get_bus(pipeline);
    auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    std::string failure;
    if (message) {
        GError *e{};
        gst_message_parse_error(message, &e, nullptr);
        failure = e ? e->message : "pipeline error";
        g_clear_error(&e);
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    require(failure.empty(), failure + " in " + description);
}
static bool has_bt709_limited(GstBuffer *buffer) {
    GstMapInfo map{};
    require(gst_buffer_map(buffer, &map, GST_MAP_READ), "Map encoded buffer");
    auto *parser = gst_h264_nal_parser_new();
    bool ok = false;
    for (size_t offset = 0; offset < map.size;) {
        GstH264NalUnit nalu{};
        const auto result = gst_h264_parser_identify_nalu(parser, map.data, offset, map.size, &nalu);
        if (result != GST_H264_PARSER_OK && result != GST_H264_PARSER_NO_NAL_END)
            break;
        if (nalu.type == GST_H264_NAL_SPS) {
            GstH264SPS sps{};
            if (gst_h264_parse_sps(&nalu, &sps) == GST_H264_PARSER_OK) {
                const auto &vui = sps.vui_parameters;
                ok = sps.vui_parameters_present_flag && vui.colour_description_present_flag &&
                     vui.colour_primaries == 1 && vui.transfer_characteristics == 1 &&
                     vui.matrix_coefficients == 1 && !vui.video_full_range_flag;
                gst_h264_sps_clear(&sps);
            }
            break;
        }
        offset = nalu.offset + nalu.size;
    }
    gst_h264_nal_parser_free(parser);
    gst_buffer_unmap(buffer, &map);
    return ok;
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    try {
        const std::string source =
            "videotestsrc num-buffers=60 ! video/x-raw,width=1280,height=720,framerate=30/1 ! "
            "vapostproc ! ";
        struct Codec {
            const char *name, *encoder, *media;
        };
        for (const auto &codec : {Codec{"h264", "vah264enc", "video/x-h264"},
                                  Codec{"h265", "vah265enc", "video/x-h265"},
                                  Codec{"av1", "vaav1enc", "video/x-av1"}}) {
            auto *factory = gst_element_factory_find(codec.encoder);
            if (!factory) {
                std::cout << "skip " << codec.name << ": " << codec.encoder << " unavailable\n";
                continue;
            }
            gst_object_unref(factory);
            for (const char *format : {"NV12", "BGRA"}) {
                Result result;
                run(source + "video/x-raw(memory:VAMemory),format=" + format +
                        " ! amdencode codec=" + codec.name + " bitrate=3000",
                    result);
                // vapostproc consumes the first buffer of a live videotestsrc start on
                // some drivers; the encoder itself must not lose any further access unit.
                require(result.frames >= 59 && result.frames <= 60, std::string(codec.name) + "/" + format +
                                                 " produced " + std::to_string(result.frames) +
                                                 " access units");
                require(result.media_type == codec.media, "Unexpected output " + result.media_type);
                if (std::string(codec.name) == "h264")
                    require(has_bt709_limited(result.first),
                            std::string("H.264 SPS does not signal BT.709 limited (") + format + ")");
                std::cout << "ok " << codec.name << " " << format << "\n";
            }
        }
        {
            Result result;
            run(source + "video/x-raw(memory:VAMemory),format=NV12 ! amdencode signal-bt709=false",
                result);
            require(result.frames >= 59 && !has_bt709_limited(result.first),
                    "signal-bt709=false must keep the encoder SPS");
            std::cout << "ok h264 signal-bt709=false\n";
        }
        if (argc == 3) {
            std::filesystem::path detect = argv[2];
            Result result;
            run("filesrc location=" + std::string(argv[1]) +
                    " ! h264parse ! video/x-h264,framerate=30/1 ! vah264dec ! "
                    "video/x-raw(memory:VAMemory),format=NV12 ! amddetect model=" +
                    (detect / "yolov8n-b1.mxr").string() +
                    " model-info=" + (detect / "model-info.json").string() +
                    " ! amdwatermark ! amdencode",
                result);
            require(result.frames >= 1 && result.media_type == "video/x-h264" &&
                        has_bt709_limited(result.first),
                    "Annotated DMA-BUF encode failed");
            std::cout << "ok annotated DMA-BUF encode (" << result.frames << " frames)\n";
        }
    } catch (const std::exception &e) {
        std::cerr << "encode-tests: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
