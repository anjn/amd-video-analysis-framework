#include <amdva/gst_telemetry.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: telemetry RTSP_URI [SECONDS] [h264|h265]\n";
        return 1;
    }
    gst_init(&argc, &argv);
    GstElement *pipeline = nullptr;
    std::unique_ptr<amdva::PipelineTelemetry> adapter;
    try {
        const unsigned seconds = argc >= 3 ? std::stoul(argv[2]) : 30;
        const std::string codec = argc == 4 ? argv[3] : "h264";
        if (codec != "h264" && codec != "h265")
            throw std::runtime_error("Expected h264 or h265");
        const auto description =
            "rtspsrc name=source protocols=tcp latency=100 ! rtp" + codec + "depay ! " + codec +
            "parse name=parser ! va" + codec +
            "dec name=decoder ! video/x-raw(memory:VAMemory),format=NV12 ! fakesink sync=false";
        GError *error{};
        pipeline = gst_parse_launch(description.c_str(), &error);
        if (error) {
            std::string why = error->message;
            g_clear_error(&error);
            throw std::runtime_error(why);
        }
        auto collector = std::make_shared<amdva::Telemetry>("camera");
        collector->begin_generation(1);
        adapter = std::make_unique<amdva::PipelineTelemetry>(collector);
        auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
        g_object_set(source, "location", argv[1], nullptr);
        adapter->attach_rtsp(source);
        gst_object_unref(source);
        auto *parser = gst_bin_get_by_name(GST_BIN(pipeline), "parser");
        auto *pad = gst_element_get_static_pad(parser, "src");
        adapter->attach_access_units(pad);
        gst_object_unref(pad);
        gst_object_unref(parser);
        auto *decoder = gst_bin_get_by_name(GST_BIN(pipeline), "decoder");
        adapter->attach_decoder(decoder);
        gst_object_unref(decoder);
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Pipeline start failed");
        auto *bus = gst_element_get_bus(pipeline);
        for (unsigned i = 0; i < seconds; i++) {
            if (auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
                GError *why{};
                gst_message_parse_error(message, &why, nullptr);
                std::string text = why ? why->message : "Pipeline error";
                g_clear_error(&why);
                gst_message_unref(message);
                gst_object_unref(bus);
                throw std::runtime_error(text);
            }
            std::cout << collector->snapshot().json() << '\n' << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        adapter.reset();
        gst_object_unref(pipeline);
        return 0;
    } catch (const std::exception &e) {
        if (pipeline)
            gst_element_set_state(pipeline, GST_STATE_NULL);
        adapter.reset();
        if (pipeline)
            gst_object_unref(pipeline);
        std::cerr << e.what() << '\n';
        return 2;
    }
}
