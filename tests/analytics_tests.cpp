#include "amdva/model.hpp"
#include "amdva/overlay.hpp"
#include "amdva/tensor_meta.hpp"
#include <cmath>
#include <gst/analytics/analytics.h>
#include <iostream>
using namespace amdva;
static void require(bool v, const char *why) {
    if (!v)
        throw std::runtime_error(why);
}
static DeviceTensor tensor(std::vector<size_t> shape, const std::vector<float> &values) {
    DeviceTensor t{"output0", TensorType::Float32, shape,
                   std::make_shared<DeviceBuffer>(values.size() * sizeof(float)), 0};
    require(t.elements() == values.size(), "fixture shape");
    hip_check(hipMemcpy(t.storage->data(), values.data(), t.storage->size(), hipMemcpyHostToDevice),
              "fixture upload");
    return t;
}
int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    try {
        auto geometry = ImageTransform::make(1920, 1080, 640, 640);
        require(geometry.scaled_width == 640 && geometry.scaled_height == 360 &&
                    geometry.pad_y == 140,
                "Letterbox geometry");
        auto box = geometry.to_source(0, 140, 640, 360);
        require(box[0] == 0 && box[1] == 0 && box[2] == 1920 && box[3] == 1080, "Inverse geometry");
        auto roi = ImageTransform::make(1920, 1080, 224, 224, false, 100, 200, 300, 400);
        box = roi.to_source(0, 0, 224, 224);
        require(box[0] == 100 && box[1] == 200 && box[2] == 300 && box[3] == 400,
                "ROI inverse geometry");
        ModelInfo info;
        info.task = "classification";
        info.classes = 3;
        info.top_k = 2;
        info.labels = {"a", "b", "c"};
        ModelPostprocessor classify(info);
        auto r = classify.process({tensor({2, 3}, {0, 1, 2, 3, 0, 1})}, 2);
        require(r[0].classifications.size() == 2 && r[0].classifications[0].class_id == 2 &&
                    r[1].classifications[0].class_id == 0,
                "Top-k/batch isolation");
        require(std::abs(r[0].classifications[0].score - .66524096f) < 1e-5, "Softmax");
        info.task = "detection";
        info.classes = 2;
        info.labels = {"a", "b"};
        std::vector<float> raw = {10, 10, 50, 60, 10,  10,  50,  60,  8,   8,   6,    6,
                                  8,  8,  6,  6,  .9f, .8f, .1f, .1f, .1f, .1f, .95f, .8f};
        ModelPostprocessor detect(info);
        r = detect.process({tensor({1, 6, 4}, raw)}, 1);
        require(r[0].objects.size() == 3 && r[0].objects[0].box.class_id == 1 &&
                    r[0].objects[1].box.class_id == 0,
                "Class-aware NMS");
        auto *buffer = gst_buffer_new();
        attach_analysis(buffer, r[0], info, ImageTransform::make(100, 100, 100, 100),
                        {"camera", 7, 9, 123});
        auto *meta = get_analysis_meta(buffer);
        require(meta && (**meta->data).identity.generation == 7, "Frame identity");
        auto *clone = gst_buffer_copy(buffer);
        require(get_analysis_meta(clone) && get_analysis_meta(clone)->data != meta->data,
                "Independent mutable metadata");
        (**get_analysis_meta(clone)->data).attachments.clear();
        require((**meta->data).attachments.size() == 1, "Branch metadata isolation");
        gst_buffer_unref(clone);
        gst_buffer_unref(buffer);
        info.task = "semantic-segmentation";
        info.classes = 3;
        info.labels = {"a", "b", "c"};
        ModelPostprocessor semantic(info);
        r = semantic.process({tensor({1, 3, 2, 2}, {3, 0, 0, 4, 0, 3, 0, 1, 1, 1, 3, 0})}, 1);
        int labels[4];
        hip_check(hipMemcpy(labels, r[0].tensors[0].storage->data(), sizeof(labels),
                            hipMemcpyDeviceToHost),
                  "offline mask reference");
        require(labels[0] == 0 && labels[1] == 1 && labels[2] == 2 && labels[3] == 0,
                "Semantic argmax");
        info.task = "pose";
        info.classes = 1;
        info.keypoints = 2;
        info.labels = {"person"};
        ModelPostprocessor pose(info);
        r = pose.process({tensor({1, 11, 1}, {10, 20, 8, 10, .9f, 8, 18, .8f, 12, 22, .7f})}, 1);
        require(r[0].objects.size() == 1 && r[0].objects[0].keypoints.size() == 2 &&
                    r[0].objects[0].keypoints[1].x == 12,
                "Pose extraction");
        info.task = "instance-segmentation";
        info.width = 2;
        info.height = 2;
        ModelPostprocessor segment(info);
        r = segment.process(
            {tensor({1, 6, 1}, {1, 1, 2, 2, .9f, 2}), tensor({1, 1, 2, 2}, {1, 0, -1, 2})}, 1);
        float masks[4];
        hip_check(
            hipMemcpy(masks, r[0].tensors[0].storage->data(), sizeof(masks), hipMemcpyDeviceToHost),
            "offline instance mask reference");
        require(r[0].objects[0].mask_index == 0 && std::abs(masks[0] - .880797f) < 1e-5 &&
                    std::abs(masks[1] - .5f) < 1e-5 && std::abs(masks[2] - .119203f) < 1e-5,
                "Prototype mask reconstruction");
        // Exercise GPU attachment merge, ROI-ID remapping and lifetime after source release.
        {
            auto *source = gst_buffer_new();
            auto *target = gst_buffer_new();
            auto data = std::make_shared<AnalysisData>();
            data->identity = {"merge", 7, 1, 123};
            auto gpu = tensor({1, 3}, {1, 2, 3});
            std::weak_ptr<DeviceBuffer> owner = gpu.storage;
            AnalysisResult raw_result;
            raw_result.tensors.push_back(std::move(gpu));
            data->attachments.push_back(
                {std::move(raw_result),
                 ImageTransform::make(100, 100, 10, 10, false, 10, 20, 40, 50), 1});
            auto *meta = add_analysis_meta(source, std::move(data));
            auto *ids = g_hash_table_new(g_direct_hash, g_direct_equal);
            g_hash_table_insert(ids, GUINT_TO_POINTER(1), GUINT_TO_POINTER(10));
            AnalysisMerge merge{ids, 2, 3};
            require(meta->meta.info->transform_func(
                        target, &meta->meta, source,
                        g_quark_from_static_string("amd-analytics-merge"), &merge),
                    "GPU attachment merge");
            const auto &copied = (**get_analysis_meta(target)->data).attachments[0];
            require(copied.parent_id == 10 && copied.geometry.crop_x == 20 &&
                        copied.geometry.crop_y == 60 && copied.geometry.crop_width == 80,
                    "Merge ROI geometry/ID remapping");
            gst_buffer_unref(source);
            require(!owner.expired(), "Aggregate released GPU payload too early");
            g_hash_table_unref(ids);
            gst_buffer_unref(target);
            require(owner.expired(), "Aggregate leaked GPU payload");
        }
        // Reference-only readback of a drawn surface verifies kernels rather than
        // merely observing that a drawing element negotiated its output caps.
        {
            VaSession va("/dev/dri/renderD128");
            auto surface = va.create_bgra(100, 100);
            {
                ImportedSurface image(va.export_surface(surface, true));
                hip_check(
                    hipMemset(const_cast<uint8_t *>(image.data()), 0, image.layout().object_size),
                    "Overlay fixture clear");
                OverlayRenderer renderer;
                OverlayFrame frame{100, 100, {{{2, 2, 10, 10, .9f, 0}, "", 0, {}}}, {}};
                renderer.draw(image, 100, 100, frame);
                uint32_t pixel{};
                hip_check(hipMemcpy(&pixel,
                                    image.data() + image.layout().y_offset +
                                        2 * image.layout().y_pitch + 2 * 4,
                                    4, hipMemcpyDeviceToHost),
                          "Offline box pixel");
                require(pixel == 0xff33ff1a, "GPU box pixel reference");
                hip_check(
                    hipMemset(const_cast<uint8_t *>(image.data()), 0, image.layout().object_size),
                    "Overlay fixture clear");
                frame.objects.clear();
                DeviceTensor mask{"semantic-mask",
                                  TensorType::Int32,
                                  {2, 2},
                                  std::make_shared<DeviceBuffer>(16),
                                  0};
                int values[] = {1, 1, 1, 1};
                hip_check(hipMemcpy(mask.storage->data(), values, 16, hipMemcpyHostToDevice),
                          "Overlay mask fixture");
                AnalysisResult result;
                result.tensors.push_back(mask);
                frame.attachments.push_back(
                    {result, ImageTransform::make(100, 100, 100, 100, false), -1});
                renderer.draw(image, 100, 100, frame);
                unsigned char color[4];
                hip_check(hipMemcpy(color,
                                    image.data() + image.layout().y_offset +
                                        50 * image.layout().y_pitch + 50 * 4,
                                    4, hipMemcpyDeviceToHost),
                          "Offline semantic overlay pixel");
                require(color[0] == 71 && color[1] == 39 && color[2] == 53 && color[3] == 255,
                        "GPU mask blend reference");
            }
            va.destroy_surface(surface);
        }
        bool rejected = false;
        try {
            segment.process({tensor({1, 6, 1}, {1, 1, 2, 2, .9f, 2})}, 1);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Malformed model output accepted");
        std::cout << "GPU classification, class-aware NMS, semantic/instance masks, pose, ROI "
                     "coordinates and metadata lifetime passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
