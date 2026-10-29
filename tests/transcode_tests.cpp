// Re-encodes generated H.264 and H.265 camera streams to H.264, H.265 and AV1
// segments and checks the recordings: catalogued, decodable on the VA device,
// scaled, and timed from the supplied receive time.
#include "amdva/streaming/playback.hpp"
#include "amdva/streaming/transcoder.hpp"
#include <filesystem>
#include <gst/app/gstappsink.h>
#include <gst/va/gstvadisplay_drm.h>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace amdva::streaming;
namespace {
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
GstElement *launch(const std::string &description) {
    GError *error{};
    auto *pipeline = gst_parse_launch(description.c_str(), &error);
    if (error) {
        std::string text = error->message;
        g_clear_error(&error);
        throw std::runtime_error(text);
    }
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    return pipeline;
}
GstElement *sink(GstElement *pipeline) {
    return gst_bin_get_by_name(GST_BIN(pipeline), "out");
}
// Decodes a finished recording; returns the frame count and the decoded height.
std::pair<unsigned, int> decode(const std::filesystem::path &file, RecordCodec codec) {
    const char *decoder = codec == RecordCodec::H264   ? "h264parse ! vah264dec"
                          : codec == RecordCodec::H265 ? "h265parse ! vah265dec"
                                                       : "av1parse ! vaav1dec";
    auto *pipeline = launch("filesrc location=\"" + file.string() + "\" ! matroskademux ! " +
                            decoder + " ! appsink name=out sync=false");
    auto *out = sink(pipeline);
    unsigned frames = 0;
    int height = 0;
    while (auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(out), 5 * GST_SECOND)) {
        if (!frames)
            gst_structure_get_int(gst_caps_get_structure(gst_sample_get_caps(sample), 0),
                                  "height", &height);
        frames++;
        gst_sample_unref(sample);
    }
    gst_object_unref(out);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return {frames, height};
}
} // namespace
int main() {
    initialize_runtime();
    const auto root =
        std::filesystem::temp_directory_path() / ("amdva-transcode-" + std::to_string(getpid()));
    auto *display = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    try {
        require(display, "Missing VA display");
        for (auto codec : {RecordCodec::H264, RecordCodec::H265, RecordCodec::AV1}) {
            const auto name = codec == RecordCodec::H264   ? "h264"
                              : codec == RecordCodec::H265 ? "h265"
                                                           : "av1";
            Catalog catalog(root / name);
            // A 1080p camera stream with PTS that signals BT.709 like the cameras (the
            // player requires it); AV1 output also exercises H.265 input. H.264 and H.265
            // are scaled to 720p; AV1 keeps 1080 lines, snapped to 1088.
            const auto input_codec = codec == RecordCodec::AV1 ? "h265" : "h264";
            auto *source = launch(std::string("videotestsrc num-buffers=90 pattern=ball ! "
                                              "video/x-raw,width=1920,height=1080,framerate=30/1"
                                              " ! vapostproc ! video/x-raw(memory:VAMemory),"
                                              "format=NV12,colorimetry=bt709 ! amdencode codec=") +
                                  input_codec + " ! appsink name=out sync=false");
            auto *input = sink(source);
            std::unique_ptr<Transcoder> transcoder;
            std::unique_ptr<SegmentRecorder> recorder;
            GstClockTime first = GST_CLOCK_TIME_NONE;
            const int64_t received = 1'700'000'000'000;
            unsigned encoded = 0, shared = 0;
            // H.265 output also offers the decoded frames to a display consumer.
            const bool share = codec == RecordCodec::H265;
            auto drain = [&] {
                while (auto *sample = transcoder->pull()) {
                    if (!recorder)
                        recorder = std::make_unique<SegmentRecorder>(
                            catalog, SegmentRecorder::Options{"camera", "Camera", {}, codec, 60},
                            gst_sample_get_caps(sample));
                    if (first == GST_CLOCK_TIME_NONE)
                        first = GST_BUFFER_PTS(gst_sample_get_buffer(sample));
                    if (!recorder->push(sample, received)) {
                        recorder->poll();
                        auto *b = gst_sample_get_buffer(sample);
                        throw std::runtime_error(
                            "Recorder refused a sample: pts=" + std::to_string(GST_BUFFER_PTS(b)) +
                            " first=" + std::to_string(first) + " error=" + recorder->error() +
                            " caps=" + gst_caps_to_string(gst_sample_get_caps(sample)));
                    }
                    encoded++;
                    gst_sample_unref(sample);
                }
                auto *offered = share ? GST_APP_SINK(transcoder->frames()) : nullptr;
                while (offered)
                    if (auto *frame = gst_app_sink_try_pull_sample(offered, 0)) {
                        shared++;
                        gst_sample_unref(frame);
                    } else
                        break;
                recorder &&recorder->poll();
                require(transcoder->poll(), "Transcoder failed: " + transcoder->error());
            };
            unsigned pushed = 0;
            while (auto *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(input), GST_SECOND)) {
                if (!transcoder) {
                    transcoder = std::make_unique<Transcoder>(
                        display,
                        Transcoder::Options{codec, RateControl::VBR, 1500,
                                            codec == RecordCodec::AV1 ? 0u : 720u, 30,
                                            16ull << 20, share},
                        gst_sample_get_caps(sample));
                    // H.264 output also checks the frame-rate cap: half of 30 fps.
                    if (codec == RecordCodec::H264)
                        transcoder->max_fps(15);
                }
                require(transcoder->push(sample), "Transcoder refused a sample");
                // Camera pace: the encoder queue drops frames when fed faster than real time.
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
                pushed++;
                gst_sample_unref(sample);
                drain();
            }
            const unsigned expected = codec == RecordCodec::H264 ? (pushed + 1) / 2 : pushed;
            for (auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                 std::chrono::steady_clock::now() < deadline && encoded < expected;)
                drain(), std::this_thread::sleep_for(std::chrono::milliseconds(10));
            gst_object_unref(input);
            gst_element_set_state(source, GST_STATE_NULL);
            gst_object_unref(source);
            require(recorder != nullptr && encoded + 2 >= expected && encoded <= expected + 2,
                    std::string(name) + ": encoded " + std::to_string(encoded) + " of " +
                        std::to_string(pushed));
            require(recorder->wall_time_ms(first) == received,
                    "Recording does not start at the receive time");
            require(transcoder->latency_ms() > 0, "Missing transcode latency");
            require(!share || shared > 0,
                    "Decoded frames were not offered");
            recorder->end();
            while (!recorder->poll())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto recordings = catalog.list();
            require(recordings.size() == 1 && recordings.front().state == "finalized",
                    std::string(name) + ": recording not finalized");
            const auto [frames, height] = decode(root / name / recordings.front().name, codec);
            require(frames + 2 >= expected && height == (codec == RecordCodec::AV1 ? 1088 : 720),
                    std::string(name) + ": decoded " + std::to_string(frames) + " frames at " +
                        std::to_string(height) + " lines");
            {
                // The NVR player and timeline previews open recordings this way.
                PlaybackSession playback(display);
                playback.open(catalog.open_recording(recordings.front().name));
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (playback.status().frames < 3 && playback.status().state != "error" &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                require(playback.status().frames >= 3,
                        std::string(name) + ": PlaybackSession " + playback.status().state + " " +
                            playback.status().error);
                playback.stop_and_release().wait();
            }
            std::cout << name << ": " << pushed << " frames, " << frames << " decoded at "
                      << height << " lines, " << recordings.front().size << " bytes, latency "
                      << transcoder->latency_ms() << " ms" << std::endl;
        }
        gst_object_unref(display);
        std::filesystem::remove_all(root);
        std::cout << "H.264, H.265 and AV1 transcoded recordings passed\n";
        return 0;
    } catch (const std::exception &e) {
        if (display)
            gst_object_unref(display);
        std::filesystem::remove_all(root);
        std::cerr << e.what() << "\n";
        return 1;
    }
}
