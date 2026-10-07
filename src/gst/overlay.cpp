#include "amdva/overlay.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <gst/analytics/analytics.h>
namespace {
// Five-column glyphs, row-major; intentionally limited to labels and IDs.
const char *glyph(char c) {
    static const char *digits[] = {
        "01110100011001110101110011000101110", "00100011000010000100001000010001110",
        "01110100010000100010001000100011111", "11110000010000101110000010000111110",
        "00010001100101010010111110001000010", "11111100001000011110000010000111110",
        "01110100001000011110100011000101110", "11111000010001000100010000100001000",
        "01110100011000101110100011000101110", "01110100011000101111000010000101110"};
    static const char *letters[] = {
        "01110100011000111111100011000110001", "11110100011000111110100011000111110",
        "01111100001000010000100001000001111", "11110100011000110001100011000111110",
        "11111100001000011110100001000011111", "11111100001000011110100001000010000",
        "01111100001000010111100011000101111", "10001100011000111111100011000110001",
        "01110001000010000100001000010001110", "00111000100001000010100101001001100",
        "10001100101010011000101001001010001", "10000100001000010000100001000011111",
        "10001110111010110101100011000110001", "10001110011010110011100011000110001",
        "01110100011000110001100011000101110", "11110100011000111110100001000010000",
        "01110100011000110001101011001001101", "11110100011000111110101001001010001",
        "01111100001000001110000010000111110", "11111001000010000100001000010000100",
        "10001100011000110001100011000101110", "10001100011000110001100010101000100",
        "10001100011000110101101011101110001", "10001100010101000100010101000110001",
        "10001100010101000100001000010000100", "11111000010001000100010001000011111"};
    c = char(std::toupper(static_cast<unsigned char>(c)));
    if (c >= '0' && c <= '9')
        return digits[c - '0'];
    if (c >= 'A' && c <= 'Z')
        return letters[c - 'A'];
    if (c == '#')
        return "01010111110101001010111110101000000";
    return nullptr;
}
const char *source = R"(
struct Line{int x0,y0,x1,y1;unsigned color;};
extern "C" __global__ void lines(unsigned char* image,unsigned offset,unsigned pitch,unsigned width,unsigned height,const Line* commands,unsigned count,unsigned stride){
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x,object=i/stride,step=i%stride;if(object>=count)return;Line l=commands[object];int dx=l.x1-l.x0,dy=l.y1-l.y0,steps=max(abs(dx),abs(dy));if(step>(unsigned)steps)return;
 int x=l.x0+(steps?dx*(int)step/steps:0),y=l.y0+(steps?dy*(int)step/steps:0);
 for(int yy=y;yy<=y+1;yy++)for(int xx=x;xx<=x+1;xx++)if(xx>=0&&yy>=0&&xx<(int)width&&yy<(int)height)atomicExch((unsigned*)(image+offset+yy*pitch+xx*4),l.color);
}
extern "C" __global__ void mask(unsigned char* image,unsigned offset,unsigned pitch,unsigned width,unsigned height,const void* raw,int semantic,unsigned mw,unsigned mh,unsigned plane,unsigned original_width,unsigned original_height,
 unsigned crop_x,unsigned crop_y,unsigned crop_width,unsigned crop_height,unsigned model_width,unsigned model_height,unsigned scaled_width,unsigned scaled_height,unsigned pad_x,unsigned pad_y){
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=width*height)return;unsigned x=i%width,y=i/width;
 float ox=(x+.5f)*original_width/width,oy=(y+.5f)*original_height/height;if(ox<crop_x||oy<crop_y||ox>=crop_x+crop_width||oy>=crop_y+crop_height)return;
 unsigned mx=min(mw-1,(unsigned)(((ox-crop_x)*scaled_width/crop_width+pad_x)*mw/model_width)),my=min(mh-1,(unsigned)(((oy-crop_y)*scaled_height/crop_height+pad_y)*mh/model_height));
 int cls=semantic?((const int*)raw)[my*mw+mx]:(int)plane;if(!semantic&&((const float*)raw)[plane*mw*mh+my*mw+mx]<.5f)return;
 unsigned char* pixel=image+offset+y*pitch+x*4;unsigned red=64+(cls*97)%192,green=64+(cls*53)%192,blue=64+(cls*151)%192;
 pixel[0]=(pixel[0]*2+blue)/3;pixel[1]=(pixel[1]*2+green)/3;pixel[2]=(pixel[2]*2+red)/3;pixel[3]=255;
})";
} // namespace
namespace amdva {
OverlayFrame overlay_from_buffer(GstBuffer *buffer, unsigned width, unsigned height) {
    OverlayFrame frame{width, height, {}, {}};
    auto *meta = gst_buffer_get_analytics_relation_meta(buffer);
    gpointer cursor = nullptr;
    GstAnalyticsMtd item{};
    while (meta && gst_analytics_relation_meta_iterate(
                       meta, &cursor, gst_analytics_od_mtd_get_mtd_type(), &item)) {
        auto *od = reinterpret_cast<GstAnalyticsODMtd *>(&item);
        gint x, y, w, h;
        gfloat confidence;
        gst_analytics_od_mtd_get_location(od, &x, &y, &w, &h, &confidence);
        OverlayObject object;
        object.box = {float(x), float(y), float(w), float(h), confidence, 0};
        auto *label = g_quark_to_string(gst_analytics_od_mtd_get_obj_type(od));
        object.label = label ? label : "";
        gpointer related = nullptr;
        GstAnalyticsMtd tracking{};
        if (gst_analytics_relation_meta_get_direct_related(
                meta, item.id, GST_ANALYTICS_REL_TYPE_ANY,
                gst_analytics_tracking_mtd_get_mtd_type(), &related, &tracking))
            gst_analytics_tracking_mtd_get_info(
                reinterpret_cast<GstAnalyticsTrackingMtd *>(&tracking), &object.tracking_id,
                nullptr, nullptr, nullptr);
        gpointer classes = nullptr;
        GstAnalyticsMtd cls{};
        float best = 0;
        std::string classification;
        while (gst_analytics_relation_meta_get_direct_related(
            meta, item.id, GST_ANALYTICS_REL_TYPE_ANY, gst_analytics_cls_mtd_get_mtd_type(),
            &classes, &cls)) {
            if (g_strcmp0(gst_analytics_mtd_get_semantic_tag(&cls), "class_descriptor") == 0)
                continue;
            auto *value = reinterpret_cast<GstAnalyticsClsMtd *>(&cls);
            for (unsigned i = 0; i < gst_analytics_cls_mtd_get_length(value); i++)
                if (gst_analytics_cls_mtd_get_level(value, i) > best) {
                    best = gst_analytics_cls_mtd_get_level(value, i);
                    const auto *text = g_quark_to_string(gst_analytics_cls_mtd_get_quark(value, i));
                    classification = text ? text : "";
                }
        }
        if (!classification.empty())
            object.label += " " + classification;
        frame.objects.push_back(std::move(object));
    }
    cursor = nullptr;
    while (meta && gst_analytics_relation_meta_iterate(
                       meta, &cursor, gst_analytics_cls_mtd_get_mtd_type(), &item)) {
        if (g_strcmp0(gst_analytics_mtd_get_semantic_tag(&item), "class_descriptor") == 0)
            continue;
        gpointer related = nullptr;
        GstAnalyticsMtd parent{};
        if (gst_analytics_relation_meta_get_direct_related(
                meta, item.id, GST_ANALYTICS_REL_TYPE_ANY, gst_analytics_od_mtd_get_mtd_type(),
                &related, &parent))
            continue;
        auto *cls = reinterpret_cast<GstAnalyticsClsMtd *>(&item);
        float best = 0;
        const char *label = nullptr;
        for (unsigned i = 0; i < gst_analytics_cls_mtd_get_length(cls); i++)
            if (gst_analytics_cls_mtd_get_level(cls, i) > best) {
                best = gst_analytics_cls_mtd_get_level(cls, i);
                label = g_quark_to_string(gst_analytics_cls_mtd_get_quark(cls, i));
            }
        if (label && frame.labels.size() < 8)
            frame.labels.push_back(label);
    }
    if (auto *meta = get_analysis_meta(buffer); meta && meta->data) {
        frame.attachments = (**meta->data).attachments;
        for (const auto &a : frame.attachments)
            for (const auto &object : a.result.objects)
                if (!object.keypoints.empty()) {
                    auto found = std::find_if(frame.objects.begin(), frame.objects.end(),
                                              [&](const auto &o) {
                                                  return std::abs(o.box.x - object.box.x) < 2 &&
                                                         std::abs(o.box.y - object.box.y) < 2;
                                              });
                    if (found != frame.objects.end())
                        found->keypoints = object.keypoints;
                }
    }
    return frame;
}
std::vector<OverlayLine> make_overlay_lines(const OverlayFrame &frame, unsigned width,
                                            unsigned height) {
    if (!frame.width || !frame.height || !width || !height)
        throw std::runtime_error("Invalid overlay dimensions");
    std::vector<OverlayLine> lines;
    const float sx = float(width) / frame.width, sy = float(height) / frame.height;
    auto line = [&](float x0, float y0, float x1, float y1) {
        if (lines.size() < 8192 && std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) &&
            std::isfinite(y1))
            lines.push_back({int(std::clamp(x0, 0.f, float(width - 1))),
                             int(std::clamp(y0, 0.f, float(height - 1))),
                             int(std::clamp(x1, 0.f, float(width - 1))),
                             int(std::clamp(y1, 0.f, float(height - 1)))});
    };
    static const unsigned edges[][2] = {{0, 1},   {0, 2},   {1, 3},   {2, 4},  {5, 6},  {5, 7},
                                        {7, 9},   {6, 8},   {8, 10},  {5, 11}, {6, 12}, {11, 12},
                                        {11, 13}, {13, 15}, {12, 14}, {14, 16}};
    auto text = [&](std::string label, float tx, float ty) {
        label = label.substr(0, 48);
        for (char c : label) {
            if (auto *bits = glyph(c))
                for (unsigned row = 0; row < 7; row++)
                    for (unsigned col = 0; col < 5;) {
                        if (bits[row * 5 + col] != '1') {
                            col++;
                            continue;
                        }
                        auto begin = col;
                        while (col + 1 < 5 && bits[row * 5 + col + 1] == '1')
                            col++;
                        line(tx + begin, ty + row, tx + col, ty + row);
                        col++;
                    }
            tx += 6;
        }
    };
    for (unsigned i = 0; i < frame.labels.size() && i < 8; i++)
        text(frame.labels[i], 2, 2 + i * 9);
    for (const auto &object : frame.objects) {
        auto b = object.box;
        float x = b.x * sx, y = b.y * sy, r = (b.x + b.w) * sx, t = (b.y + b.h) * sy;
        line(x, y, r, y);
        line(r, y, r, t);
        line(r, t, x, t);
        line(x, t, x, y);
        std::string label = object.label;
        if (object.tracking_id)
            label += " #" + std::to_string(object.tracking_id);
        text(label, x, std::max(0.f, y - 9));
        for (const auto &k : object.keypoints)
            if (k.confidence >= .25f) {
                line(k.x * sx - 1, k.y * sy, k.x * sx + 1, k.y * sy);
                line(k.x * sx, k.y * sy - 1, k.x * sx, k.y * sy + 1);
            }
        if (object.keypoints.size() == 17)
            for (const auto &edge : edges) {
                auto a = object.keypoints[edge[0]], b = object.keypoints[edge[1]];
                if (a.confidence >= .25f && b.confidence >= .25f)
                    line(a.x * sx, a.y * sy, b.x * sx, b.y * sy);
            }
    }
    return lines;
}
struct OverlayRenderer::Impl {
    int device;
    Kernel lines, mask;
    DeviceBuffer commands;
    Impl(int d)
        : device(d), lines(source, "lines", gpu_arch(d)), mask(source, "mask", gpu_arch(d)),
          commands(8192 * sizeof(OverlayLine)) {}
};
OverlayRenderer::OverlayRenderer(int device) : impl_(std::make_unique<Impl>(device)) {}
OverlayRenderer::~OverlayRenderer() = default;
void OverlayRenderer::draw(ImportedSurface &image, unsigned width, unsigned height,
                           const OverlayFrame &frame) {
    auto &s = *impl_;
    hip_check(hipSetDevice(s.device), "Overlay device");
    const auto &layout = image.layout();
    if (!width || !height || width > layout.width || height > layout.height)
        throw std::runtime_error("Overlay exceeds surface");
    auto *pixels = const_cast<uint8_t *>(image.data());
    auto offset = layout.y_offset, pitch = layout.y_pitch;
    struct Guard {
        ~Guard() { (void)hipDeviceSynchronize(); }
    } guard;
    for (const auto &attachment : frame.attachments)
        for (const auto &tensor : attachment.result.tensors) {
            int semantic = tensor.name == "semantic-mask";
            if (!semantic && tensor.name != "instance-masks")
                continue;
            if (!tensor.storage || tensor.device != s.device ||
                (semantic ? (tensor.shape.size() != 2 || tensor.type != TensorType::Int32)
                          : (tensor.shape.size() != 3 || tensor.type != TensorType::Float32)))
                throw std::runtime_error("Invalid GPU overlay mask");
            if (tensor.elements() > 64 * 1024 * 1024 ||
                tensor.elements() * sizeof(float) != tensor.storage->size())
                throw std::runtime_error("Invalid mask storage size");
            hipPointerAttribute_t location{};
            hip_check(hipPointerGetAttributes(&location, tensor.storage->data()),
                      "Overlay mask location");
            if (location.type != hipMemoryTypeDevice || location.device != s.device)
                throw std::runtime_error("Mask must reside on overlay GPU");
            const auto &g = attachment.geometry;
            unsigned mw = tensor.shape.back(), mh = tensor.shape[tensor.shape.size() - 2],
                     count = semantic ? 1 : tensor.shape[0];
            void *raw = tensor.storage->data();
            auto ow = frame.width, oh = frame.height, cx = g.crop_x, cy = g.crop_y,
                 cw = g.crop_width, ch = g.crop_height, tw = g.width, th = g.height,
                 sw = g.scaled_width, sh = g.scaled_height, px = g.pad_x, py = g.pad_y;
            if (!mw || !mh || !cw || !ch || !tw || !th)
                throw std::runtime_error("Invalid mask geometry");
            for (unsigned plane = 0; plane < count; plane++) {
                void *args[] = {&pixels, &offset, &pitch, &width, &height, &raw, &semantic, &mw,
                                &mh,     &plane,  &ow,    &oh,    &cx,     &cy,  &cw,       &ch,
                                &tw,     &th,     &sw,    &sh,    &px,     &py};
                s.mask.launch(width * height, args);
            }
        }
    auto commands = make_overlay_lines(frame, width, height);
    if (!commands.empty()) {
        upload_metadata(s.commands.data(), commands.data(), commands.size() * sizeof(OverlayLine));
        void *command = s.commands.data();
        unsigned count = commands.size(), stride = std::max(width, height) + 1;
        void *args[] = {&pixels, &offset, &pitch, &width, &height, &command, &count, &stride};
        s.lines.launch(count * stride, args);
    }
    hip_check(hipDeviceSynchronize(), "GPU overlay completion");
}
} // namespace amdva
