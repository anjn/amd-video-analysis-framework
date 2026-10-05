#include "amdva/inference.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <json-glib/json-glib.h>
#include <migraphx/migraphx.hpp>
namespace {
constexpr unsigned max_detections = 300;
using Detection = amdva::Detection;
struct Results {
    unsigned count;
    Detection boxes[max_detections];
};
static_assert(sizeof(Detection) == 24);
static const char *post_source = R"(
struct Box {float x,y,w,h,score; int cls;};
struct Result {unsigned count; Box boxes[300];};
extern "C" __global__ void decode(const float* raw, unsigned n, float threshold, Box* boxes) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
 if(i>=n) return;
 Box b; b.x=raw[i];b.y=raw[n+i];b.w=raw[2*n+i];b.h=raw[3*n+i];
 b.score=0;b.cls=0;
 for(unsigned c=0;c<80;c++) {float p=raw[(4+c)*n+i];if(p>b.score){b.score=p;b.cls=c;}}
 b.x-=b.w/2;b.y-=b.h/2;
 if(!isfinite(b.x)||!isfinite(b.y)||!isfinite(b.w)||!isfinite(b.h)||!isfinite(b.score)||b.w<=0||b.h<=0||b.score<threshold) b.score=0;
 boxes[i]=b;
}
extern "C" __global__ void nms(Box* boxes,unsigned n,float threshold,Result* result) {
 if(blockIdx.x || threadIdx.x) return;
 result->count=0;
 for(unsigned k=0;k<300;k++) {
  int best=-1; float score=0;
  for(unsigned i=0;i<n;i++) if(boxes[i].score>score){best=i;score=boxes[i].score;}
  if(best<0) break;
  Box b=boxes[best]; result->boxes[result->count++]=b;boxes[best].score=0;
  for(unsigned i=0;i<n;i++) {
   Box c=boxes[i]; if(c.score<=0||c.cls!=b.cls) continue;
   float w=fmaxf(0,fminf(b.x+b.w,c.x+c.w)-fmaxf(b.x,c.x));
   float h=fmaxf(0,fminf(b.y+b.h,c.y+c.h)-fmaxf(b.y,c.y));
   float intersection=w*h,area=b.w*b.h+c.w*c.h-intersection;
   if(area>0 && intersection/area>threshold) boxes[i].score=0;
  }
 }
})";
struct Parser {
    JsonParser *parser{json_parser_new()};
    explicit Parser(const std::string &path) {
        GError *e = nullptr;
        if (!json_parser_load_from_file(parser, path.c_str(), &e)) {
            const std::string message = e ? e->message : "JSON load failed";
            g_clear_error(&e);
            g_object_unref(parser);
            parser = nullptr;
            throw std::runtime_error(message);
        }
        if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
            g_object_unref(parser);
            parser = nullptr;
            throw std::runtime_error("JSON root must be an object");
        }
    }
    ~Parser() {
        if (parser)
            g_object_unref(parser);
    }
    JsonObject *object() const { return json_node_get_object(json_parser_get_root(parser)); }
};
static const char *required_string(JsonObject *o, const char *key) {
    if (!json_object_has_member(o, key) ||
        json_node_get_value_type(json_object_get_member(o, key)) != G_TYPE_STRING)
        throw std::runtime_error(std::string("Missing string: ") + key);
    return json_object_get_string_member(o, key);
}
static JsonObject *required_object(JsonObject *o, const char *key) {
    if (!json_object_has_member(o, key) || !JSON_NODE_HOLDS_OBJECT(json_object_get_member(o, key)))
        throw std::runtime_error(std::string("Missing object: ") + key);
    return json_object_get_object_member(o, key);
}
static JsonArray *required_array(JsonObject *o, const char *key) {
    if (!json_object_has_member(o, key) || !JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, key)))
        throw std::runtime_error(std::string("Missing array: ") + key);
    return json_object_get_array_member(o, key);
}
static std::string sha256_file(const char *path) {
    gchar *data{};
    gsize size{};
    GError *e = nullptr;
    if (!g_file_get_contents(path, &data, &size, &e)) {
        std::string error = e ? e->message : "Read failed";
        g_clear_error(&e);
        throw std::runtime_error(error);
    }
    gchar *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                              reinterpret_cast<const guchar *>(data), size);
    std::string result = hash;
    g_free(hash);
    g_free(data);
    return result;
}
} // namespace
namespace amdva {
struct BatchEngine::Impl {
    migraphx::program program;
    std::string input_name;
    migraphx::shape input_shape;
    std::vector<std::string> labels;
    std::vector<std::pair<std::string, std::unique_ptr<amdva::DeviceBuffer>>> bindings;
    std::unique_ptr<amdva::Kernel> decode, nms;
    std::unique_ptr<amdva::DeviceBuffer> boxes, result;
    unsigned candidates{};
    unsigned width{}, height{}, batch{};
};
BatchEngine::BatchEngine(const std::string &model, const std::string &info_path, unsigned batch,
                         int device)
    : device_(device) {
    if (batch != 1 && batch != 2 && batch != 4 && batch != 8 && batch != 16)
        throw std::runtime_error("Batch must be 1,2,4,8 or16");
    hip_check(hipSetDevice(device), "Inference device selection");
    const auto arch = amdva::gpu_arch(device);
    Parser manifest(std::string(model.c_str()) + ".manifest.json");
    auto *m = manifest.object();
    if (!json_object_has_member(m, "schema") ||
        json_node_get_value_type(json_object_get_member(m, "schema")) != G_TYPE_INT64 ||
        json_object_get_int_member(m, "schema") != 1 ||
        !json_object_has_member(m, "offload_copy") ||
        json_node_get_value_type(json_object_get_member(m, "offload_copy")) != G_TYPE_BOOLEAN ||
        json_object_get_boolean_member(m, "offload_copy") || arch != required_string(m, "gpu") ||
        sha256_file(model.c_str()) != required_string(m, "sha256"))
        throw std::runtime_error(
            "Model manifest is incompatible: GPU, hash or device I/O contract");
    auto state = std::make_unique<Impl>();
    Parser info(info_path.c_str());
    auto *config = info.object();
    if (std::string(required_string(config, "family")) != "yolov8")
        throw std::runtime_error("PoC supports YOLOv8 COCO detection only");
    auto *input = required_object(config, "input");
    auto *output = required_object(config, "output");
    if (!input || !output || std::string(required_string(input, "format")) != "NCHW" ||
        std::string(required_string(input, "channel_order")) != "RGB" ||
        std::string(required_string(output, "format")) != "cxcywh" ||
        json_object_get_int_member(output, "num_classes") != 80)
        throw std::runtime_error("Unsupported YOLO input/output convention");
    auto *normalize = required_array(input, "normalize");
    if (json_array_get_length(normalize) != 2 || json_array_get_double_element(normalize, 0) != 0 ||
        json_array_get_double_element(normalize, 1) != 1)
        throw std::runtime_error("Expected RGB normalization to [0,1]");
    auto *shape = required_array(input, "shape");
    if (!shape || json_array_get_length(shape) != 4 || json_array_get_int_element(shape, 0) != 1 ||
        json_array_get_int_element(shape, 1) != 3)
        throw std::runtime_error("Expected static [1,3,H,W] input");
    state->height = json_array_get_int_element(shape, 2);
    state->width = json_array_get_int_element(shape, 3);
    if (state->height != 640 || state->width != 640)
        throw std::runtime_error("Initial model shape is fixed to 640x640");
    gchar *dir = g_path_get_dirname(info_path.c_str());
    const auto labels_path = std::string(dir) + "/" + required_string(config, "labels");
    g_free(dir);
    std::ifstream labels(labels_path);
    std::string line;
    while (std::getline(labels, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        state->labels.push_back(line);
    }
    if (state->labels.size() != 80)
        throw std::runtime_error("Expected 80 COCO labels relative to model-info");
    state->program = migraphx::load(model.c_str());
    auto params = state->program.get_parameter_shapes();
    for (const char *name : params.names()) {
        auto sh = params[name];
        if (sh.type() == migraphx_shape_float_type &&
            sh.lengths() == std::vector<size_t>{batch, 3, 640, 640}) {
            if (!state->input_name.empty())
                throw std::runtime_error("Ambiguous model image input");
            state->input_name = name;
            state->input_shape = sh;
        } else if (std::string(name).find("#output_") != std::string::npos &&
                   sh.type() == migraphx_shape_float_type &&
                   sh.lengths() == std::vector<size_t>{batch, 84, 8400}) {
            state->bindings.emplace_back(name, std::make_unique<amdva::DeviceBuffer>(sh.bytes()));
        } else
            throw std::runtime_error(std::string("Unsupported additional model input: ") + name);
    }
    if (state->input_name.empty())
        throw std::runtime_error("Missing float32 NCHW input");
    auto outputs = state->program.get_output_shapes();
    if (outputs.size() != 1 || outputs[0].type() != migraphx_shape_float_type ||
        outputs[0].lengths() != std::vector<size_t>{batch, 84, 8400} ||
        outputs[0].strides() != std::vector<size_t>{705600, 8400, 1})
        throw std::runtime_error("Expected contiguous YOLOv8 [1,84,8400] float output");
    state->candidates = 8400;
    state->batch = batch;
    state->decode = std::make_unique<amdva::Kernel>(post_source, "decode", arch);
    state->nms = std::make_unique<amdva::Kernel>(post_source, "nms", arch);
    state->boxes = std::make_unique<amdva::DeviceBuffer>(8400 * sizeof(Detection));
    state->result = std::make_unique<amdva::DeviceBuffer>(sizeof(Results));
    impl_ = std::move(state);
}
BatchEngine::~BatchEngine() {
    if (impl_) {
        (void)hipSetDevice(device_);
        (void)hipDeviceSynchronize();
    }
}
const std::vector<std::string> &BatchEngine::labels() const { return impl_->labels; }
std::vector<std::vector<Detection>> BatchEngine::infer(void *input, unsigned valid,
                                                       float confidence, float nms_threshold) {
    auto &state = *impl_;
    const int device = device_;
    if (!input || valid < 1 || valid > state.batch)
        throw std::runtime_error("Invalid batch occupancy");
    hip_check(hipSetDevice(device), "Inference device selection");
    migraphx::program_parameters params;
    params.add(state.input_name.c_str(), migraphx::argument(state.input_shape, input));
    auto shapes = state.program.get_parameter_shapes();
    for (auto &binding : state.bindings)
        params.add(binding.first.c_str(),
                   migraphx::argument(shapes[binding.first.c_str()], binding.second->data()));
    auto outputs = state.program.eval(params);
    // Device-I/O evaluation may enqueue work on MIGraphX's non-blocking
    // stream. Default-stream postprocessing does not implicitly wait for it.
    // Complete inference before reading outputs or releasing the input owner.
    amdva::hip_check(hipDeviceSynchronize(), "MIGraphX device-I/O completion");
    auto *raw = reinterpret_cast<float *>(outputs[0].data());
    hipPointerAttribute_t attrs{};
    amdva::hip_check(hipPointerGetAttributes(&attrs, raw), "Model output pointer check");
    if (attrs.type != hipMemoryTypeDevice || attrs.device != device)
        throw std::runtime_error("MIGraphX output is not on the selected GPU");
    std::vector<std::vector<Detection>> detections(valid);
    for (unsigned slot = 0; slot < valid; ++slot) {
        auto *image = raw + slot * 84 * 8400;
        unsigned n = state.candidates;
        void *boxes = state.boxes->data();
        void *result = state.result->data();
        void *decode_args[] = {&image, &n, &confidence, &boxes};
        state.decode->launch(n, decode_args);
        void *nms_args[] = {&boxes, &n, &nms_threshold, &result};
        state.nms->launch(1, nms_args);
        Results detected{};
        hip_check(hipMemcpy(&detected, result, sizeof(detected), hipMemcpyDeviceToHost),
                  "Compact detection readback");
        if (detected.count > max_detections)
            throw std::runtime_error("Invalid GPU detection count");
        detections[slot].assign(detected.boxes, detected.boxes + detected.count);
    }
    return detections;
}
} // namespace amdva
