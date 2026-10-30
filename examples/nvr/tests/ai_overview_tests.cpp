#include "ai_overview.hpp"
#include <QApplication>
#include <QImage>
#include <cmath>
#include <iostream>
using namespace amdva::nvr;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        std::vector<Status> statuses(2);
        std::vector<amdva::TelemetrySnapshot> telemetry(2);
        for (int i = 0; i < 2; ++i) {
            auto &s = statuses[i];
            s.source.id = i ? "b" : "a";
            s.source.name = i ? "Lobby" : "Gate";
            s.connection = "live";
            s.input_codec = "h265";
            s.input_width = 1920;
            s.input_height = 1080;
            s.gaps = i;
            auto &t = telemetry[i];
            t.source = s.source.id;
            t.enabled = true;
            auto &decoded = t.stages[0][unsigned(amdva::TimingStage::DecodeOutput)];
            decoded.supported = true;
            decoded.fps = 30;
            t.latency[0][0].samples = 10;
            t.latency[0][0].p99_ms = i ? 12 : 8;
        }
        auto metric = [](const char *stage, const char *camera, double rate, double batch) {
            AiMetric m;
            m.stage = stage;
            m.camera = camera;
            m.rate = rate;
            m.mean_batch = batch;
            m.mean_ms = 5;
            return m;
        };
        std::vector<AiMetric> metrics{metric("YOLO inference", "", 2, 4),
                                      metric("ReID inference + transfer", "a", 1, 2),
                                      metric("ReID inference + transfer", "", 3, 2),
                                      metric("Image embedding", "", .5, 1),
                                      metric("Image embedding NPU", "", 1, 1),
                                      metric("Tracking association", "", 4, 1)};
        const auto now = Clock::now();
        std::map<std::string, PersonTrackFrame> tracks;
        auto &a = tracks["a"];
        a.generation = 1;
        a.captured = now - std::chrono::milliseconds(200);
        a.objects.resize(2);
        a.objects[0].person = 7;
        a.objects[1].person = 8;
        auto &b = tracks["b"];
        b.generation = 1;
        b.captured = now - std::chrono::milliseconds(100);
        b.objects.resize(1);
        b.objects[0].person = 7; // Same identity seen by another camera.
        Options options;
        options.search.enabled = true;

        auto o = summarize_overview(statuses, telemetry, metrics, tracks, options, false, now);
        check(near(o.decode.fps, 60), "Decode frame rate sums cameras");
        check(near(o.decode.mpixels, 60 * 1920 * 1080 / 1e6), "Decoded pixel rate");
        check(o.decode.live == 2 && o.decode.cameras == 2 && o.decode.gaps == 1, "Camera counts");
        check(near(o.decode.p99_ms, 12), "Worst camera P99");
        check(o.decode.formats == QStringList{"H265 1920×1080 ×2"}, "Input formats");
        check(o.decode.state == AiOverviewSample::Good, "Decode healthy");
        check(near(o.gpu.yolo_images, 8), "YOLO images = calls × batch");
        check(near(o.gpu.reid_crops, 6), "ReID uses the all-camera row");
        check(near(o.gpu.embedding_images, .5), "EmbeddingGemma runs on the GPU");
        check(!o.npu.configured && o.npu.state == AiOverviewSample::Idle, "NPU unused");
        check(o.tracking.people == 3 && o.tracking.identities == 2, "People and identities");
        check(near(o.tracking.rate, 4) && near(o.tracking.age_ms, 200), "Tracking rate and age");
        check(o.tracking.cameras.size() == 2 && o.tracking.cameras[1].first == "Lobby" &&
                  o.tracking.cameras[1].second == 1,
              "Per-camera people");

        options.search.embedding_model = "mobileclip2-s2";
        options.search.mobileclip_backend = "gpu+npu";
        statuses[1].connection = "reconnecting";
        o = summarize_overview(statuses, telemetry, metrics, tracks, options, false, now);
        check(o.npu.configured && near(o.npu.images, 1) && o.npu.state == AiOverviewSample::Good,
              "NPU active");
        check(near(o.gpu.embedding_images, 0), "MobileCLIP GPU row is separate");
        check(o.decode.state == AiOverviewSample::Warning, "Reconnecting camera warns");
        o = summarize_overview(statuses, telemetry, metrics, tracks, options, true, now);
        check(o.decode.state == AiOverviewSample::Error && o.gpu.state == AiOverviewSample::Error,
              "Fatal error");

        AiOverview widget;
        widget.resize(800, 500);
        for (int i = 0; i < 70; ++i)
            widget.push(o);
        check(widget.history() == AiOverview::history_limit, "History is bounded");
        const auto image = widget.grab().toImage();
        check(!image.isNull() && image.pixelColor(200, 125) != image.pixelColor(2, 2),
              "Cards are painted");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
