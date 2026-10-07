#include "amdva/preprocess.hpp"
#include <algorithm>
#include <cmath>
namespace {
const char *source = R"(
extern "C" __global__ void pack(const unsigned char* bgra,unsigned offset,unsigned pitch,
 unsigned sw,unsigned sh,unsigned tw,unsigned th,unsigned px,unsigned py,float* out,
 int bgr,float scale,float padding,float m0,float m1,float m2,float s0,float s1,float s2) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=tw*th)return;
 unsigned x=i%tw,y=i/tw;float r=padding,g=r,b=r;
 if(x>=px&&x<px+sw&&y>=py&&y<py+sh){const unsigned char* p=bgra+offset+(y-py)*pitch+(x-px)*4;b=p[0];g=p[1];r=p[2];}
 out[i]=((bgr?b:r)*scale-m0)/s0;out[tw*th+i]=(g*scale-m1)/s1;out[2*tw*th+i]=((bgr?r:b)*scale-m2)/s2;
}
extern "C" __global__ void pad(float* out,unsigned valid,unsigned batch,unsigned size,
 float padding,float scale,float m0,float m1,float m2,float s0,float s1,float s2) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=(batch-valid)*size*3)return;
 unsigned c=(i/size)%3;float m=c==0?m0:c==1?m1:m2,s=c==0?s0:c==1?s1:s2;
 out[valid*size*3+i]=(padding*scale-m)/s;
})";
void validate(const amdva::PixelNormalization &n) {
    if (!std::isfinite(n.scale) || !std::isfinite(n.padding))
        throw std::runtime_error("Invalid normalization");
    for (unsigned i = 0; i < 3; i++)
        if (!std::isfinite(n.mean[i]) || !std::isfinite(n.stddev[i]) || n.stddev[i] <= 0)
            throw std::runtime_error("Invalid normalization mean/stddev");
}
} // namespace
namespace amdva {
ImageTransform ImageTransform::make(unsigned sw, unsigned sh, unsigned tw, unsigned th,
                                    bool letterbox, unsigned x, unsigned y, unsigned w,
                                    unsigned h) {
    if (!sw || !sh || !tw || !th || tw > 4096 || th > 4096 || x >= sw || y >= sh)
        throw std::runtime_error("Invalid preprocessing dimensions");
    if (!w)
        w = sw - x;
    if (!h)
        h = sh - y;
    if (w > sw - x || h > sh - y)
        throw std::runtime_error("ROI exceeds source bounds");
    ImageTransform t{sw, sh, tw, th, tw, th, 0, 0, x, y, w, h};
    if (letterbox) {
        double f = std::min(double(tw) / w, double(th) / h);
        t.scaled_width = std::min(tw, std::max(1u, unsigned(std::lround(w * f))));
        t.scaled_height = std::min(th, std::max(1u, unsigned(std::lround(h * f))));
        t.pad_x = (tw - t.scaled_width) / 2;
        t.pad_y = (th - t.scaled_height) / 2;
    }
    return t;
}
std::array<float, 4> ImageTransform::to_source(float x, float y, float w, float h) const {
    const float sx = float(crop_width) / scaled_width, sy = float(crop_height) / scaled_height;
    const float left =
        std::clamp((x - pad_x) * sx + crop_x, float(crop_x), float(crop_x + crop_width));
    const float top =
        std::clamp((y - pad_y) * sy + crop_y, float(crop_y), float(crop_y + crop_height));
    const float right =
        std::clamp((x + w - pad_x) * sx + crop_x, float(crop_x), float(crop_x + crop_width));
    const float bottom =
        std::clamp((y + h - pad_y) * sy + crop_y, float(crop_y), float(crop_y + crop_height));
    return {left, top, std::max(0.f, right - left), std::max(0.f, bottom - top)};
}
ImagePacker::ImagePacker(int device)
    : device_(device), pack_(source, "pack", gpu_arch(device)),
      pad_(source, "pad", gpu_arch(device)) {}
void ImagePacker::pack(const ImportedSurface &image, const ImageTransform &t, void *out,
                       const PixelNormalization &n, hipStream_t stream) {
    validate(n);
    hip_check(hipSetDevice(device_), "Pack device");
    const auto &l = image.layout();
    if (!out || l.width < t.scaled_width || l.height < t.scaled_height)
        throw std::runtime_error("Invalid pack surface/destination");
    auto pixels = image.data();
    auto offset = l.y_offset, pitch = l.y_pitch;
    auto sw = t.scaled_width, sh = t.scaled_height;
    auto tw = t.width, th = t.height, px = t.pad_x, py = t.pad_y;
    int bgr = n.bgr;
    auto scale = n.scale, padding = n.padding;
    auto mean = n.mean, stddev = n.stddev;
    void *args[] = {&pixels,  &offset,  &pitch,     &sw,        &sh,       &tw,      &th,
                    &px,      &py,      &out,       &bgr,       &scale,    &padding, &mean[0],
                    &mean[1], &mean[2], &stddev[0], &stddev[1], &stddev[2]};
    pack_.launch(tw * th, args, stream);
}
void ImagePacker::pad(void *out, unsigned valid, unsigned batch, unsigned width, unsigned height,
                      const PixelNormalization &n, hipStream_t stream) {
    validate(n);
    hip_check(hipSetDevice(device_), "Pad device");
    if (!out || valid > batch || !width || !height || width > 4096 || height > 4096 || batch > 16)
        throw std::runtime_error("Invalid padding dimensions");
    if (valid == batch)
        return;
    unsigned size = width * height;
    auto padding = n.padding, scale = n.scale;
    auto mean = n.mean, stddev = n.stddev;
    void *args[] = {&out,     &valid,   &batch,   &size,      &padding,   &scale,
                    &mean[0], &mean[1], &mean[2], &stddev[0], &stddev[1], &stddev[2]};
    pad_.launch((batch - valid) * 3 * size, args, stream);
}
} // namespace amdva
