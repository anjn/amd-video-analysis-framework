#include "amdva/model.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace {
constexpr unsigned limit = 300;
struct Candidate {
    amdva::Detection box;
    unsigned index;
};
struct Detections {
    unsigned count;
    Candidate items[limit];
};
struct TopK {
    unsigned count;
    amdva::Classification items[100];
};
const char *source = R"(
struct Box{float x,y,w,h,score;int cls;};struct Candidate{Box box;unsigned index;};
struct Result{unsigned count;Candidate items[300];};struct Class{int cls;float score;};struct TopK{unsigned count;Class items[100];};
extern "C" __global__ void decode(const float* raw,unsigned n,unsigned classes,float threshold,Candidate* out){
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n)return;
 Box b;b.x=raw[i];b.y=raw[n+i];b.w=raw[2*n+i];b.h=raw[3*n+i];b.score=0;b.cls=0;
 for(unsigned c=0;c<classes;c++){float p=raw[(4+c)*n+i];if(p>b.score){b.score=p;b.cls=c;}}
 b.x-=b.w/2;b.y-=b.h/2;if(!isfinite(b.x)||!isfinite(b.y)||!isfinite(b.w)||!isfinite(b.h)||!isfinite(b.score)||b.w<=0||b.h<=0||b.score<threshold)b.score=0;
 out[i]={b,i};
}
extern "C" __global__ void nms(Candidate* boxes,unsigned n,float threshold,Result* result){
 if(blockIdx.x||threadIdx.x)return;result->count=0;
 for(unsigned k=0;k<300;k++){int best=-1;float score=0;for(unsigned i=0;i<n;i++)if(boxes[i].box.score>score){best=i;score=boxes[i].box.score;}
 if(best<0)break;Candidate candidate=boxes[best];Box b=candidate.box;result->items[result->count++]=candidate;boxes[best].box.score=0;
 for(unsigned i=0;i<n;i++){Box c=boxes[i].box;if(c.score<=0||c.cls!=b.cls)continue;float w=fmaxf(0,fminf(b.x+b.w,c.x+c.w)-fmaxf(b.x,c.x)),h=fmaxf(0,fminf(b.y+b.h,c.y+c.h)-fmaxf(b.y,c.y));float inter=w*h,area=b.w*b.h+c.w*c.h-inter;if(area>0&&inter/area>threshold)boxes[i].box.score=0;}}
}
extern "C" __global__ void topk(const float* raw,unsigned classes,unsigned k,TopK* out){
 if(blockIdx.x||threadIdx.x)return;float maximum=-__builtin_inff();for(unsigned i=0;i<classes;i++)if(isfinite(raw[i]))maximum=fmaxf(maximum,raw[i]);
 float total=0;for(unsigned i=0;i<classes;i++)if(isfinite(raw[i]))total+=expf(raw[i]-maximum);out->count=0;if(!(total>0))return;
 for(unsigned j=0;j<k&&j<classes;j++){int best=-1;float score=-__builtin_inff();for(unsigned i=0;i<classes;i++){bool used=false;for(unsigned t=0;t<j;t++)if(out->items[t].cls==(int)i)used=true;if(!used&&isfinite(raw[i])&&raw[i]>score){best=i;score=raw[i];}}if(best<0)break;out->items[out->count++]={best,expf(score-maximum)/total};}
}
extern "C" __global__ void semantic(const float* raw,unsigned classes,unsigned size,int* out){
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=size)return;int best=0;float score=-__builtin_inff();for(unsigned c=0;c<classes;c++)if(raw[c*size+i]>score){score=raw[c*size+i];best=c;}out[i]=best;
}
extern "C" __global__ void masks(const float* raw,const float* proto,const Result* detections,unsigned n,unsigned classes,unsigned channels,unsigned width,unsigned height,unsigned model_width,unsigned model_height,float* out){
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;unsigned size=width*height,object=i/size,pixel=i%size;if(object>=detections->count)return;
 Candidate d=detections->items[object];float x=(pixel%width+.5f)*model_width/width,y=(pixel/width+.5f)*model_height/height;
 if(x<d.box.x||y<d.box.y||x>=d.box.x+d.box.w||y>=d.box.y+d.box.h){out[i]=0;return;}
 float v=0;for(unsigned c=0;c<channels;c++)v+=raw[(4+classes+c)*n+d.index]*proto[c*size+pixel];out[i]=1.f/(1.f+expf(-v));
}
extern "C" __global__ void pose(const float* raw,const Result* detections,unsigned n,unsigned classes,unsigned points,float* out){
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;unsigned object=i/(points*3),c=i%(points*3);if(object>=detections->count)return;out[i]=raw[(4+classes+c)*n+detections->items[object].index];
})";
void validate(const amdva::DeviceTensor &t, int device) {
    if (t.device != device || t.type != amdva::TensorType::Float32 || !t.storage ||
        t.elements() * sizeof(float) != t.storage->size())
        throw std::runtime_error("Postprocessing requires contiguous GPU float32 tensors");
    hipPointerAttribute_t a{};
    amdva::hip_check(hipPointerGetAttributes(&a, t.storage->data()),
                     "Postprocessing input location");
    if (a.type != hipMemoryTypeDevice || a.device != device)
        throw std::runtime_error("Postprocessing input is not device memory");
}
} // namespace
namespace amdva {
struct ModelPostprocessor::Impl {
    ModelInfo info;
    int device;
    Kernel decode, nms, topk, semantic, masks, pose;
    std::unique_ptr<DeviceBuffer> candidates, compact, keypoints;
    Impl(ModelInfo m, int d)
        : info(std::move(m)), device(d), decode(source, "decode", gpu_arch(d)),
          nms(source, "nms", gpu_arch(d)), topk(source, "topk", gpu_arch(d)),
          semantic(source, "semantic", gpu_arch(d)), masks(source, "masks", gpu_arch(d)),
          pose(source, "pose", gpu_arch(d)) {
        compact = std::make_unique<DeviceBuffer>(sizeof(Detections));
    }
};
ModelPostprocessor::ModelPostprocessor(ModelInfo info, int device)
    : impl_(std::make_unique<Impl>(std::move(info), device)) {
    const auto &m = impl_->info;
    if (m.task != "raw" && m.task != "classification" && m.task != "semantic-segmentation" &&
        m.task != "detection" && m.task != "instance-segmentation" && m.task != "pose")
        throw std::runtime_error("Unsupported model task: " + m.task);
    if ((m.task == "detection" || m.task == "instance-segmentation" || m.task == "pose") &&
        m.adapter != "yolov8" && m.adapter != "yolov8-seg" && m.adapter != "yolov8-pose")
        throw std::runtime_error("Unsupported detection adapter: " + m.adapter);
}
ModelPostprocessor::~ModelPostprocessor() = default;
std::vector<AnalysisResult> ModelPostprocessor::process(const std::vector<DeviceTensor> &outputs,
                                                        unsigned valid, float confidence,
                                                        float threshold) {
    auto &s = *impl_;
    const auto &m = s.info;
    hip_check(hipSetDevice(s.device), "Postprocessing device");
    if (outputs.empty() || !valid || valid > 16 || !std::isfinite(confidence) ||
        !std::isfinite(threshold) || confidence < 0 || confidence > 1 || threshold < 0 ||
        threshold > 1)
        throw std::runtime_error("Invalid postprocessing arguments");
    std::vector<AnalysisResult> results(valid);
    for (unsigned i = 0; i < valid; i++)
        results[i].batch_index = i;
    if (m.task == "raw") {
        for (auto &r : results)
            r.tensors = outputs;
        return results;
    }
    for (const auto &t : outputs)
        validate(t, s.device);
    const auto &shape = outputs[0].shape;
    if (shape.empty() || shape[0] < valid)
        throw std::runtime_error("Output batch is too small");
    unsigned classes = m.classes;
    for (unsigned b = 0; b < valid; b++) {
        auto &r = results[b];
        auto *raw = static_cast<float *>(outputs[0].storage->data()) +
                    b * (outputs[0].elements() / shape[0]);
        if (m.task == "classification") {
            if (shape.size() != 2 || shape[1] != classes || outputs.size() != 1)
                throw std::runtime_error("Classification expects [B,classes] logits");
            void *out = s.compact->data();
            auto k = m.top_k;
            void *args[] = {&raw, &classes, &k, &out};
            s.topk.launch(1, args);
            TopK top{};
            readback_metadata(&top, out, sizeof(top));
            if (top.count > 100)
                throw std::runtime_error("Invalid top-k result");
            r.classifications.assign(top.items, top.items + top.count);
            continue;
        }
        if (m.task == "semantic-segmentation") {
            if (shape.size() != 4 || shape[1] != classes || outputs.size() != 1)
                throw std::runtime_error("Semantic segmentation expects [B,classes,H,W]");
            if (shape[2] > 8192 || shape[3] > 8192 || shape[2] * shape[3] > 64 * 1024 * 1024)
                throw std::runtime_error("Semantic mask dimensions exceed limits");
            unsigned size = shape[2] * shape[3];
            DeviceTensor mask{"semantic-mask",
                              TensorType::Int32,
                              {shape[2], shape[3]},
                              std::make_shared<DeviceBuffer>(size * sizeof(int)),
                              s.device};
            void *out = mask.storage->data();
            void *args[] = {&raw, &classes, &size, &out};
            s.semantic.launch(size, args);
            hip_check(hipDeviceSynchronize(), "Semantic mask completion");
            r.tensors.push_back(std::move(mask));
            continue;
        }
        if (shape.size() != 3 || shape[2] > 100000 || shape[2] == 0)
            throw std::runtime_error("YOLO expects [B,channels,candidates]");
        unsigned n = shape[2], extra = m.task == "pose" ? m.keypoints * 3 : 0, mask_channels = 0,
                 mask_width = 0, mask_height = 0;
        if (m.task == "instance-segmentation") {
            if (outputs.size() != 2 || outputs[1].shape.size() != 4 ||
                outputs[1].shape[0] != shape[0])
                throw std::runtime_error("YOLO segmentation requires mask prototypes [B,C,H,W]");
            mask_channels = outputs[1].shape[1];
            mask_height = outputs[1].shape[2];
            mask_width = outputs[1].shape[3];
            extra = mask_channels;
            if (mask_channels > 256 || mask_width > 1024 || mask_height > 1024)
                throw std::runtime_error("Mask prototype dimensions exceed limits");
        } else if (outputs.size() != 1)
            throw std::runtime_error("Unexpected additional YOLO outputs");
        if (shape[1] != 4 + classes + extra)
            throw std::runtime_error("YOLO channel count differs from model-info");
        if (!s.candidates || s.candidates->size() < n * sizeof(Candidate))
            s.candidates = std::make_unique<DeviceBuffer>(n * sizeof(Candidate));
        void *candidate = s.candidates->data();
        void *out = s.compact->data();
        void *decode_args[] = {&raw, &n, &classes, &confidence, &candidate};
        s.decode.launch(n, decode_args);
        void *nms_args[] = {&candidate, &n, &threshold, &out};
        s.nms.launch(1, nms_args);
        Detections detections{};
        readback_metadata(&detections, out, sizeof(detections));
        if (detections.count > limit)
            throw std::runtime_error("Invalid object count");
        for (unsigned j = 0; j < detections.count; j++) {
            Instance object;
            object.box = detections.items[j].box;
            r.objects.push_back(object);
        }
        if (m.task == "pose" && detections.count) {
            unsigned points = m.keypoints;
            size_t bytes = size_t(detections.count) * points * 3 * sizeof(float);
            if (!s.keypoints || s.keypoints->size() < bytes)
                s.keypoints = std::make_unique<DeviceBuffer>(bytes);
            void *target = s.keypoints->data();
            void *args[] = {&raw, &out, &n, &classes, &points, &target};
            s.pose.launch(detections.count * points * 3, args);
            std::vector<Keypoint> host(detections.count * points);
            static_assert(sizeof(Keypoint) == 3 * sizeof(float));
            readback_metadata(host.data(), target, bytes);
            for (unsigned j = 0; j < detections.count; j++)
                r.objects[j].keypoints.assign(host.begin() + j * points,
                                              host.begin() + (j + 1) * points);
        }
        if (m.task == "instance-segmentation" && detections.count) {
            // Bound mask allocation independently of untrusted model dimensions.
            size_t count = size_t(detections.count) * mask_width * mask_height;
            if (count > 64ull * 1024 * 1024)
                throw std::runtime_error("Instance mask budget exceeded");
            DeviceTensor masks{"instance-masks",
                               TensorType::Float32,
                               {detections.count, mask_height, mask_width},
                               std::make_shared<DeviceBuffer>(count * sizeof(float)),
                               s.device};
            auto *proto = static_cast<float *>(outputs[1].storage->data()) +
                          b * mask_channels * mask_width * mask_height;
            void *target = masks.storage->data();
            unsigned mw = m.width, mh = m.height;
            void *args[] = {&raw,        &proto,       &out, &n,  &classes, &mask_channels,
                            &mask_width, &mask_height, &mw,  &mh, &target};
            s.masks.launch(count, args);
            hip_check(hipDeviceSynchronize(), "Instance mask completion");
            for (unsigned j = 0; j < detections.count; j++)
                r.objects[j].mask_index = j;
            r.tensors.push_back(std::move(masks));
        }
    }
    return results;
}
} // namespace amdva
