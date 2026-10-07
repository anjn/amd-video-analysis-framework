#include "amdva/model.hpp"
#include "amdva/overlay.hpp"
#include "amdva/publishing.hpp"
#include "amdva/video_preprocess.hpp"
#include <fstream>
#include <gst/app/gstappsink.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <iostream>
#include <json-glib/json-glib.h>
using namespace amdva;
// Offline reference tool. CPU tensor/image readback is intentional here and is
// never part of the plugin or streaming execution path.
static void dump(const std::string &path, const DeviceBuffer &buffer) {
    std::vector<char> host(buffer.size());
    hip_check(hipMemcpy(host.data(), buffer.data(), host.size(), hipMemcpyDeviceToHost),
              "Offline reference readback");
    std::ofstream f(path, std::ios::binary);
    f.write(host.data(), host.size());
    if (!f)
        throw std::runtime_error("Cannot write reference output");
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc != 5) {
        std::cerr << "Usage: amdva-validate-model INPUT.h264 MODEL.mxr MODEL.json OUTPUT_PREFIX\n";
        return 1;
    }
    GstElement *pipeline{}, *sink{};
    GstSample *sample{};
    auto cleanup = [&] {
        if (sample)
            gst_sample_unref(sample);
        if (pipeline)
            gst_element_set_state(pipeline, GST_STATE_NULL);
        if (sink)
            gst_object_unref(sink);
        if (pipeline)
            gst_object_unref(pipeline);
    };
    try {
        GError *error = nullptr;
        pipeline = gst_parse_launch(
            "filesrc name=input ! h264parse ! vah264dec ! video/x-raw(memory:VAMemory),format=NV12 "
            "! appsink name=output sync=false max-buffers=1 drop=true",
            &error);
        if (error) {
            std::string why = error->message;
            g_clear_error(&error);
            throw std::runtime_error(why);
        }
        auto *input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
        g_object_set(input, "location", argv[1], nullptr);
        gst_object_unref(input);
        sink = gst_bin_get_by_name(GST_BIN(pipeline), "output");
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Decode start failed");
        sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
        if (!sample)
            throw std::runtime_error("Decoded reference frame timed out");
        auto info = ModelInfo::load(argv[3]);
        GstVideoInfo video{};
        if (!gst_video_info_from_caps(&video, gst_sample_get_caps(sample)))
            throw std::runtime_error("Invalid decoded caps");
        auto *buffer = gst_sample_get_buffer(sample);
        VideoPreprocessor preprocess(info);
        auto tensor = preprocess.prepare(buffer, video);
        std::string prefix = argv[4];
        dump(prefix + ".input.f32", *tensor->buffer);
        ModelExecutor model(argv[2]);
        if (model.batch() != 1)
            throw std::runtime_error("Reference validation requires batch one");
        DeviceTensor image{info.input_name,
                           TensorType::Float32,
                           {1, 3, info.height, info.width},
                           tensor->buffer,
                           0};
        auto outputs = model.infer({{image.name, image}});
        std::ofstream descriptor(prefix + ".shapes.json");
        descriptor << '[';
        for (unsigned i = 0; i < outputs.size(); i++) {
            if (outputs[i].type != TensorType::Float32)
                throw std::runtime_error("Reference output must be float32");
            dump(prefix + ".output" + std::to_string(i) + ".f32", *outputs[i].storage);
            if (i)
                descriptor << ',';
            descriptor << '[';
            for (unsigned d = 0; d < outputs[i].shape.size(); d++) {
                if (d)
                    descriptor << ',';
                descriptor << outputs[i].shape[d];
            }
            descriptor << ']';
        }
        descriptor << "]\n";
        descriptor.close();
        ModelPostprocessor post(info);
        auto results = post.process(outputs, 1);
        for (const auto &mask : results[0].tensors)
            dump(prefix + "." + mask.name + (mask.type == TensorType::Int32 ? ".i32" : ".f32"),
                 *mask.storage);
        auto *annotated = gst_buffer_copy(buffer);
        attach_analysis(annotated, std::move(results[0]), info, tensor->geometry,
                        {"reference", 1, 0, GST_BUFFER_PTS(buffer)});
        std::ofstream(prefix + ".json") << analysis_json(annotated) << '\n';
        auto *display = gst_va_buffer_peek_display(buffer);
        VaSession va(static_cast<VADisplay>(gst_va_display_get_va_dpy(display)));
        auto surface = va.create_bgra(video.width, video.height);
        try {
            va.scale(gst_va_buffer_get_surface(buffer), surface, video.width, video.height,
                     video.width, video.height);
            ImportedSurface image(va.export_surface(surface, true));
            OverlayRenderer overlay;
            overlay.draw(image, video.width, video.height,
                         overlay_from_buffer(annotated, video.width, video.height));
            DeviceBuffer view(const_cast<uint8_t *>(image.data()), image.layout().object_size);
            dump(prefix + ".bgra", view);
        } catch (...) {
            va.destroy_surface(surface);
            gst_buffer_unref(annotated);
            throw;
        }
        va.destroy_surface(surface);
        gst_buffer_unref(annotated);
        cleanup();
        std::cout << "GPU model, postprocessing and overlay completed\n";
        return 0;
    } catch (const std::exception &e) {
        cleanup();
        std::cerr << e.what() << '\n';
        return 2;
    }
}
