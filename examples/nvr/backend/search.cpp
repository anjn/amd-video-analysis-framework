#include "backend/search.hpp"
#include "backend/json.hpp"
#include "backend/controller.hpp"
#include "backend/mobileclip_batch.hpp"
#include "backend/mobileclip_client.hpp"
#include "backend/reid.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <gst/va/gstva.h>
#include <png.h>
#include <random>
#include <set>
#include <sstream>
extern "C" void amdva_audit_search_begin(const void *, size_t, int) __attribute__((weak));
extern "C" void amdva_audit_search_end() __attribute__((weak));
extern "C" void amdva_audit_embedding_scope(bool) __attribute__((weak));
namespace amdva::nvr {
Detection search_crop(Detection b, unsigned w, unsigned h, float margin) {
    if (!w || !h || !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.w) ||
        !std::isfinite(b.h) || b.w <= 0 || b.h <= 0)
        throw std::invalid_argument("Empty search region");
    float x = std::clamp(std::floor(b.x - margin * b.w), 0.f, float(w));
    float y = std::clamp(std::floor(b.y - margin * b.h), 0.f, float(h));
    float r = std::clamp(std::ceil(b.x + b.w + margin * b.w), 0.f, float(w));
    float t = std::clamp(std::ceil(b.y + b.h + margin * b.h), 0.f, float(h));
    b.x = x;
    b.y = y;
    b.w = r - x;
    b.h = t - y;
    if (b.w < 1 || b.h < 1)
        throw std::invalid_argument("Search region outside video");
    return b;
}
std::pair<unsigned, unsigned> search_dimensions(unsigned w, unsigned h, unsigned limit) {
    if (!w || !h || limit < 32 || limit > 1024)
        throw std::invalid_argument("Invalid search dimensions");
    double scale = std::min({1., double(limit) / w, double(limit) / h});
    return {std::max(1u, unsigned(std::lround(w * scale))),
            std::max(1u, unsigned(std::lround(h * scale)))};
}
std::vector<uint8_t> search_letterbox(const SearchImage &im, unsigned size) {
    if (size < 32 || size > 1024 || !im.width || !im.height || im.width > size ||
        im.height > size || im.rgb.size() != size_t(im.width) * im.height * 3)
        throw std::invalid_argument("Invalid search image");
    std::vector<uint8_t> out(size_t(size) * size * 3, 0);
    for (unsigned y = 0; y < im.height; ++y)
        std::memcpy(out.data() + ((y + (size - im.height) / 2) * size + (size - im.width) / 2) * 3,
                    im.rgb.data() + size_t(y) * im.width * 3, size_t(im.width) * 3);
    return out;
}
std::vector<SearchGroup> group_search_results(std::vector<SearchObservation> hits) {
    std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) {
        return std::tie(a.channel, a.person_id, a.time_ms, a.id) <
               std::tie(b.channel, b.person_id, b.time_ms, b.id);
    });
    std::vector<SearchGroup> groups;
    for (auto &o : hits) {
        if (groups.empty() || groups.back().channel != o.channel ||
            groups.back().person_id != o.person_id ||
            (o.person_id.empty() && (o.time_ms - groups.back().to_ms > 10000 ||
                                     o.time_ms - groups.back().from_ms > 60000)))
            groups.push_back({o.channel, o.camera_name, o.time_ms, o.time_ms, o.similarity, {}});
        auto &g = groups.back();
        g.person_id = o.person_id;
        g.to_ms = o.time_ms;
        g.similarity = std::max(g.similarity, o.similarity);
        g.images.push_back(std::move(o));
    }
    for (auto &g : groups)
        std::stable_sort(g.images.begin(), g.images.end(),
                         [](const auto &a, const auto &b) { return a.similarity > b.similarity; });
    std::stable_sort(groups.begin(), groups.end(),
                     [](const auto &a, const auto &b) { return a.similarity > b.similarity; });
    if (groups.size() > 50)
        groups.resize(50);
    return groups;
}
bool prune_search_group(SearchGroup &g,
                        const std::function<bool(const std::string &)> &present) {
    const auto before = g.images.size();
    g.images.erase(std::remove_if(g.images.begin(), g.images.end(),
                                  [&](const auto &o) { return !present(o.recording); }),
                   g.images.end());
    if (g.images.size() == before)
        return false;
    if (!g.images.empty()) {
        g.from_ms = g.to_ms = g.images.front().time_ms;
        g.similarity = 0;
        for (const auto &o : g.images) {
            g.from_ms = std::min(g.from_ms, o.time_ms);
            g.to_ms = std::max(g.to_ms, o.time_ms);
            g.similarity = std::max(g.similarity, o.similarity);
        }
    }
    return true;
}
std::array<float, 768> compose_text_query(const std::array<float, 768> &positive,
                                          const std::vector<std::array<float, 768>> &negatives,
                                          float weight) {
    auto out = positive;
    for (const auto &negative : negatives)
        for (size_t i = 0; i < out.size(); ++i)
            out[i] -= weight * negative[i];
    double norm = 0;
    for (float v : out)
        norm += double(v) * v;
    if (norm <= 0)
        throw std::invalid_argument("Negative query cancels the positive query");
    const float scale = float(1 / std::sqrt(norm));
    for (auto &v : out)
        v *= scale;
    return out;
}
std::array<float, 768> compose_text_query(const std::vector<std::array<float, 768>> &phrasings,
                                          const std::vector<std::array<float, 768>> &negatives,
                                          float weight) {
    if (phrasings.empty())
        throw std::invalid_argument("Empty text query");
    std::array<float, 768> mean{};
    for (const auto &phrasing : phrasings)
        for (size_t i = 0; i < mean.size(); ++i)
            mean[i] += phrasing[i] / float(phrasings.size());
    return compose_text_query(mean, negatives, weight);
}
struct SearchCropper::Impl {
    VaSession va;
    VASurfaceID converted{VA_INVALID_SURFACE};
    std::unique_ptr<ImportedSurface> imported;
    Kernel kernel;
    DeviceBuffer rgb;
    unsigned limit;
    Impl(GstVaDisplay *display, unsigned limit, unsigned device)
        : va(static_cast<VADisplay>(gst_va_display_get_va_dpy(display))),
          kernel(R"(
extern "C" __global__ void crop_rgb(const unsigned char *src,unsigned pitch,unsigned offset,unsigned sw,unsigned sh,
 unsigned x,unsigned y,unsigned cw,unsigned ch,unsigned ow,unsigned oh,unsigned char *dst) {
 unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=ow*oh)return;
 float fx=x+((i%ow)+0.5f)*cw/ow-0.5f,fy=y+((i/ow)+0.5f)*ch/oh-0.5f;
 fx=fminf(fmaxf(fx,float(x)),float(x+cw-1));fy=fminf(fmaxf(fy,float(y)),float(y+ch-1));
 unsigned x0=(unsigned)fx,y0=(unsigned)fy,x1=min(x0+1,sw-1),y1=min(y0+1,sh-1);
 float ax=fx-x0,ay=fy-y0;
 for(unsigned c=0;c<3;c++) {unsigned k=2-c;
 float a=src[offset+y0*pitch+x0*4+k]*(1-ax)+src[offset+y0*pitch+x1*4+k]*ax;
 float b=src[offset+y1*pitch+x0*4+k]*(1-ax)+src[offset+y1*pitch+x1*4+k]*ax;
 dst[i*3+c]=(unsigned char)(a*(1-ay)+b*ay+0.5f);}
})",
                 "crop_rgb", gpu_arch(device)),
          rgb(size_t(limit) * limit * 3), limit(limit) {
        converted = va.create_bgra(max_video_width, max_video_height);
        imported = std::make_unique<ImportedSurface>(va.export_surface(converted));
    }
    ~Impl() {
        imported.reset();
        va.destroy_surface(converted);
    }
};
SearchCropper::SearchCropper(GstVaDisplay *d, unsigned n, unsigned device) {
    search_dimensions(1, 1, n);
    hip_check(hipSetDevice(device), "Search GPU");
    impl_ = std::make_unique<Impl>(d, n, device);
}
SearchCropper::~SearchCropper() = default;
SearchImage SearchCropper::crop(VASurfaceID surface, unsigned width, unsigned height, Detection b) {
    return crop_many(surface, width, height, {b}).front();
}
std::vector<SearchImage> SearchCropper::crop_many(VASurfaceID surface, unsigned width,
                                                  unsigned height,
                                                  const std::vector<Detection> &boxes) {
    if (!width || !height || width > max_video_width || height > max_video_height)
        throw std::invalid_argument("Unsupported search source size");
    std::vector<SearchImage> images;
    if (boxes.empty())
        return images;
    auto &p = *impl_;
    p.va.scale(surface, p.converted, width, height, width, height);
    va_check(vaSyncSurface(p.va.display(), p.converted), "Search surface sync");
    for (auto b : boxes) {
        b = search_crop(b, width, height);
        auto dims = search_dimensions(unsigned(b.w), unsigned(b.h), p.limit);
        SearchImage image{dims.first, dims.second, {}};
        image.rgb.resize(size_t(image.width) * image.height * 3);
        auto src = p.imported->data();
        auto layout = p.imported->layout();
        auto dst = p.rgb.data();
        unsigned x = unsigned(b.x), y = unsigned(b.y), w = unsigned(b.w), h = unsigned(b.h);
        void *args[] = {&src, &layout.y_pitch, &layout.y_offset, &width, &height, &x, &y, &w,
                        &h,   &image.width,    &image.height,    &dst};
        p.kernel.launch(image.width * image.height, args);
        if (amdva_audit_search_begin)
            amdva_audit_search_begin(dst, image.rgb.size(), int(hipMemcpyDeviceToHost));
        auto rc = hipMemcpy(image.rgb.data(), dst, image.rgb.size(), hipMemcpyDeviceToHost);
        if (amdva_audit_search_end)
            amdva_audit_search_end();
        hip_check(rc, "Search crop readback");
        images.push_back(std::move(image));
    }
    return images;
}
namespace {
std::vector<uint8_t> png_encode(const SearchImage &im) {
    png_image p{};
    p.version = PNG_IMAGE_VERSION;
    p.width = im.width;
    p.height = im.height;
    p.format = PNG_FORMAT_RGB;
    png_alloc_size_t bytes = 0;
    if (!png_image_write_to_memory(&p, nullptr, &bytes, 0, im.rgb.data(), 0, nullptr))
        throw std::runtime_error(p.message);
    std::vector<uint8_t> out(bytes);
    if (!png_image_write_to_memory(&p, out.data(), &bytes, 0, im.rgb.data(), 0, nullptr))
        throw std::runtime_error(p.message);
    out.resize(bytes);
    return out;
}
struct EmbeddingScope {
    EmbeddingScope() {
        if (amdva_audit_embedding_scope)
            amdva_audit_embedding_scope(true);
    }
    ~EmbeddingScope() {
        if (amdva_audit_embedding_scope)
            amdva_audit_embedding_scope(false);
    }
};
struct Embedder {
    std::unique_ptr<MobileClipClient> mobileclip, secondary;
    bool combined{}, next_single_npu{};
    unsigned gpu_capacity{};
    size_t last_gpu_count{};
    std::shared_ptr<AiRuntime> metrics;
    std::vector<std::array<float, 768>>
    mobile_images(MobileClipClient &client, const std::vector<SearchImage> &source, bool npu) {
        const auto start = Clock::now();
        auto result = client.images(source);
        if (metrics)
            metrics->record(npu ? "Image embedding NPU" : "Image embedding GPU", "",
                            std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                            0, source.size());
        return result;
    }
    void *library{}, *context{};
    using Create = int (*)(const char *, int, void **, char *, size_t);
    using Destroy = void (*)(void *);
    using Images = int (*)(void *, const uint8_t *, size_t, size_t, size_t, int, float *, size_t,
                           char *, size_t);
    using Texts = int (*)(void *, const char *const *, size_t, const char *, float *, size_t,
                          char *, size_t);
    Destroy destroy{};
    Images images{};
    Texts texts{};
    template <class F> F symbol(const char *n) {
        auto f = reinterpret_cast<F>(dlsym(library, n));
        if (!f)
            throw std::runtime_error(std::string("Missing libeg2 symbol: ") + n);
        return f;
    }
    explicit Embedder(const SearchOptions &o) {
        EmbeddingScope scope;
        if (o.embedding_model == "mobileclip2-s2") {
            combined = o.mobileclip_backend == "gpu+npu";
            gpu_capacity = o.embedding_batch;
            if (combined) {
                auto backend = o;
                backend.mobileclip_backend = "npu";
                secondary = std::make_unique<MobileClipClient>(backend);
                backend.mobileclip_backend = "gpu";
                mobileclip = std::make_unique<MobileClipClient>(backend);
            } else
                mobileclip = std::make_unique<MobileClipClient>(o);
            return;
        }
        library = dlopen(o.library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library)
            throw std::runtime_error(std::string("Cannot load libeg2: ") + dlerror());
        try {
            destroy = symbol<Destroy>("eg2_destroy");
            images = symbol<Images>("eg2_encode_images");
            texts = symbol<Texts>("eg2_encode_texts");
            char error[4096]{};
            if (symbol<Create>("eg2_create")(o.bundle.c_str(), o.device, &context, error,
                                             sizeof(error)))
                throw std::runtime_error(error);
        } catch (...) {
            dlclose(library);
            throw;
        }
    }
    ~Embedder() {
        EmbeddingScope scope;
        if (context)
            destroy(context);
        if (library)
            dlclose(library);
    }
    std::vector<std::array<float, 768>> encode_batch(const std::vector<SearchImage> &source,
                                                     const SearchOptions &o) {
        EmbeddingScope scope;
        if (combined) {
            last_gpu_count = mobileclip_gpu_count(source.size(), gpu_capacity, next_single_npu);
            if (source.size() == 1)
                next_single_npu = !next_single_npu;
            std::vector<SearchImage> gpu(source.begin(), source.begin() + last_gpu_count);
            std::vector<SearchImage> npu(source.begin() + last_gpu_count, source.end());
            std::future<std::vector<std::array<float, 768>>> pending_npu;
            if (!npu.empty())
                pending_npu = std::async(std::launch::async, [this, &npu] {
                    return mobile_images(*secondary, npu, true);
                });
            std::vector<std::array<float, 768>> out;
            if (!gpu.empty())
                out = mobile_images(*mobileclip, gpu, false);
            if (pending_npu.valid()) {
                auto next = pending_npu.get();
                out.insert(out.end(), next.begin(), next.end());
            }
            return out;
        }
        if (mobileclip)
            return mobile_images(*mobileclip, source, o.mobileclip_backend == "npu");
        const unsigned w = o.embedding_width ? o.embedding_width : o.image_size,
                       h = o.embedding_height ? o.embedding_height : o.image_size;
        if (source.empty())
            return {};
        if (source.size() > o.embedding_batch)
            throw std::invalid_argument("Embedding batch exceeds configured capacity");
        // Native libeg2 retains one image profile. Keep a fixed physical batch
        // to avoid multi-second reloads when the number of people changes.
        const size_t physical_batch = o.embedding_batch;
        std::vector<uint8_t> pixels(physical_batch * w * h * 3, 0);
        for (size_t n = 0; n < source.size(); ++n) {
            const auto &im = source[n];
            if (!im.width || !im.height || im.rgb.size() != size_t(im.width) * im.height * 3)
                throw std::invalid_argument("Invalid embedding image");
            double scale = std::min({1., double(w) / im.width, double(h) / im.height});
            unsigned rw = std::max(1u, unsigned(im.width * scale)),
                     rh = std::max(1u, unsigned(im.height * scale));
            for (unsigned y = 0; y < rh; ++y)
                for (unsigned x = 0; x < rw; ++x) {
                    const double fx =
                        std::clamp((x + .5) * im.width / rw - .5, 0., double(im.width - 1));
                    const double fy =
                        std::clamp((y + .5) * im.height / rh - .5, 0., double(im.height - 1));
                    unsigned x0 = fx, y0 = fy, x1 = std::min(x0 + 1, im.width - 1),
                             y1 = std::min(y0 + 1, im.height - 1);
                    for (unsigned c = 0; c < 3; ++c) {
                        auto at = [&](unsigned xx, unsigned yy) {
                            return im.rgb[(size_t(yy) * im.width + xx) * 3 + c];
                        };
                        double top = at(x0, y0) * (1 - (fx - x0)) + at(x1, y0) * (fx - x0);
                        double bottom = at(x0, y1) * (1 - (fx - x0)) + at(x1, y1) * (fx - x0);
                        pixels[((n * h + y + (h - rh) / 2) * w + x + (w - rw) / 2) * 3 + c] =
                            uint8_t(std::lround(top * (1 - (fy - y0)) + bottom * (fy - y0)));
                    }
                }
        }
        const size_t image_bytes = size_t(w) * h * 3;
        for (size_t n = source.size(); n < physical_batch; ++n)
            std::copy_n(pixels.data() + (source.size() - 1) * image_bytes, image_bytes,
                        pixels.data() + n * image_bytes);
        std::vector<std::array<float, 768>> out(physical_batch);
        char error[4096]{};
        if (images(context, pixels.data(), physical_batch, h, w, 70, out[0].data(),
                   physical_batch * 768, error, sizeof(error)))
            throw std::runtime_error(error);
        out.resize(source.size());
        return out;
    }
    std::pair<std::array<float, 768>, std::array<float, 768>>
    combined_query(const SearchImage *image, const std::string &text) {
        if (!image) {
            // Text is the same CPU FP32 encoder for both image backends.
            auto query = mobileclip->text(text);
            return {query, query};
        }
        const std::vector<SearchImage> input{*image};
        auto npu = std::async(std::launch::async, [this, &input] {
            return mobile_images(*secondary, input, true).front();
        });
        auto gpu = mobile_images(*mobileclip, input, false).front();
        return {gpu, npu.get()};
    }
    std::array<float, 768> encode(const SearchImage *im, const std::string &text, unsigned size) {
        EmbeddingScope scope;
        if (mobileclip)
            return im ? mobileclip->images({*im}).front() : mobileclip->text(text);
        std::array<float, 768> out{};
        char error[4096]{};
        int rc;
        if (im) {
            auto rgb = search_letterbox(*im, size);
            rc = images(context, rgb.data(), 1, size, size, 70, out.data(), out.size(), error,
                        sizeof(error));
        } else {
            const char *s = text.c_str();
            rc = texts(context, &s, 1, "task: search result | query: ", out.data(), out.size(),
                       error, sizeof(error));
        }
        if (rc)
            throw std::runtime_error(error);
        return out;
    }
};
std::string fingerprint(const SearchOptions &o) {
    const bool mobileclip = o.embedding_model == "mobileclip2-s2";
    const auto root = std::filesystem::path(mobileclip ? o.mobileclip_bundle : o.bundle);
    std::string input = "rgb-square-black-v1/" + std::to_string(o.image_size) + "/70/";
    if (o.embedding_width)
        input += "portrait-v1/" + std::to_string(o.embedding_width) + "x" +
                 std::to_string(o.embedding_height) + "/";
    if (mobileclip)
        input = "mobileclip2-s2/512-zero-tail-v1/letterbox-bilinear-256/" + o.mobileclip_backend +
                "/crop-limit/" + std::to_string(o.image_size) + "/";
    auto append = [&](const std::filesystem::path &path) {
        std::ifstream f(path);
        if (!f)
            throw std::runtime_error("Cannot read embedding manifest: " + path.string());
        std::ostringstream s;
        s << f.rdbuf();
        input += s.str();
    };
    if (!o.reid_model.empty()) {
        input += "osnet-person-v1/";
        append(o.reid_model);
    }
    if (mobileclip) {
        // GPU FP16 and NPU BF16 also have distinct vector spaces.
        append(root / "manifest.json");
        append(root / (o.mobileclip_backend + ".json"));
    } else {
        Parser index((root / "index.json").string());
        append(root / "index.json");
        auto *profiles = required_array(index.object(), "profiles");
        for (unsigned i = 0; i < json_array_get_length(profiles); ++i) {
            auto *profile = json_array_get_object_element(profiles, i);
            append(root / required_string(profile, "path") / "bundle.json");
        }
    }
    // Bundle manifests identify the model revision, compiled profiles and weight hashes.
    auto *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, input.c_str(), input.size());
    std::string result(hash);
    g_free(hash);
    return result;
}
} // namespace
struct SearchService::Impl {
    SearchIndex &index;
    Catalog &catalog;
    GstVaDisplay *display;
    SearchOptions options;
    std::shared_ptr<AiRuntime> runtime;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker, tracking_worker;
    bool stopping{};
    struct Job {
        Frame frame;
        std::string channel;
        std::vector<Detection> boxes;
        Clock::time_point queued = Clock::now();
    };
    struct Pending {
        SearchObservation observation;
        SearchImage image;
        Clock::time_point queued, updated;
    };
    std::deque<Job> jobs;
    std::map<std::string, Pending> pending;
    std::map<std::string, int64_t> last_saved;
    std::map<std::string, PersonTrackFrame> track_frames;
    struct Request {
        uint64_t id{};
        std::string text;
        SearchImage image;
        std::vector<std::string> negatives, alternatives;
    };
    template <class Encode>
    static std::array<float, 768> text_query(const Request &r, Encode encode) {
        std::vector<std::array<float, 768>> negatives;
        for (const auto &text : r.negatives)
            negatives.push_back(encode(text));
        std::vector<std::array<float, 768>> phrasings{encode(r.text)};
        for (const auto &text : r.alternatives)
            phrasings.push_back(encode(text));
        return negatives.empty() && phrasings.size() == 1
                   ? phrasings.front()
                   : compose_text_query(phrasings, negatives);
    }
    Request query;
    uint64_t serial{};
    SearchResult result;
    SearchStatus status;
    std::string key;
    std::string run_id =
        std::to_string(std::random_device{}()) + "-" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    Impl(SearchIndex &i, GstVaDisplay *d, SearchOptions o, std::shared_ptr<AiRuntime> r)
        : index(i), catalog(i.catalog()), display(d), options(std::move(o)),
          runtime(r ? r : std::make_shared<AiRuntime>()) {
        if (!r) {
            runtime->configure(runtime->fps(), options.interval_ms);
            runtime->mobileclip_budget(options.mobileclip_gpu_budget_percent,
                                      options.mobileclip_npu_budget_percent);
        }
        gst_object_ref(display);
        status.state = "loading";
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        stop();
        gst_object_unref(display);
    }
    void stop() {
        {
            std::lock_guard<std::mutex> l(mutex);
            stopping = true;
            jobs.clear();
            pending.clear();
            query = {};
            status.pending = 0;
            cv.notify_all();
        }
        if (worker.joinable())
            worker.join();
        if (tracking_worker.joinable())
            tracking_worker.join();
    }
    static double elapsed(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    void fail(const std::exception &e) {
        std::lock_guard<std::mutex> l(mutex);
        status.state = "error";
        status.error = e.what();
        cv.notify_all();
    }
    void track() {
        try {
            hip_check(hipSetDevice(options.device), "Tracking GPU");
            SearchCropper cropper(display, options.image_size, options.device);
            std::unique_ptr<ReIdModel> reid;
            if (!options.reid_model.empty()) {
                auto lease = runtime->acquire(AiRuntime::Tracking);
                reid = std::make_unique<ReIdModel>(options.reid_model, options.device);
                reid->encode({1, 1, {128, 128, 128}});
            }
            std::map<std::string, std::unique_ptr<PersonTracker>> trackers;
            std::map<std::string, std::string> sessions;
            {
                std::lock_guard<std::mutex> l(mutex);
                if (reid) {
                    const auto &shape = reid->input_shape();
                    status.reid_shape = std::to_string(shape[0]) + " × 3 × " +
                                        std::to_string(shape[2]) + " × " + std::to_string(shape[3]);
                }
                status.state = "ready";
                cv.notify_all();
            }
            // Jobs hold one frame per camera. A batch of them shares one GPU lease and one
            // ReID call, so fewer lease hand-offs with detection and fuller ReID batches.
            constexpr size_t batch_jobs = 8;
            for (;;) {
                std::vector<Job> batch;
                {
                    std::unique_lock<std::mutex> l(mutex);
                    cv.wait(l, [&] { return stopping || !jobs.empty(); });
                    if (stopping)
                        break;
                    while (!jobs.empty() && batch.size() < batch_jobs) {
                        batch.push_back(std::move(jobs.front()));
                        jobs.pop_front();
                    }
                    runtime->queue("Tracking queue", "", jobs.size());
                }
                std::vector<std::vector<SearchImage>> crops(batch.size());
                std::vector<std::vector<PersonDetection>> people(batch.size());
                std::vector<std::string> failed(batch.size());
                // VA decoders hand frames on before decoding finishes. Wait here, not under
                // the GPU lease, so encode-heavy video engine queues do not hold up
                // detection and the other cameras' tracking.
                for (auto &job : batch) {
                    auto synced = Clock::now();
                    auto *buffer = gst_sample_get_buffer(job.frame.sample.get());
                    va_check(vaSyncSurface(
                                 static_cast<VADisplay>(gst_va_display_get_va_dpy(display)),
                                 gst_va_buffer_get_surface(buffer)),
                             "Tracking frame decode");
                    runtime->record("Decoded frame wait", job.channel, elapsed(synced));
                }
                {
                    auto lease = runtime->acquire(AiRuntime::Tracking);
                    std::vector<SearchImage> all;
                    for (size_t j = 0; j < batch.size(); ++j) {
                        auto &job = batch[j];
                        try {
                            const double waited = elapsed(job.queued);
                            auto t = Clock::now();
                            std::vector<Detection> boxes;
                            for (auto box : job.boxes)
                                boxes.push_back(search_crop(box, job.frame.width,
                                                            job.frame.height, reid ? 0 : .1f));
                            crops[j] = cropper.crop_many(
                                gst_va_buffer_get_surface(
                                    gst_sample_get_buffer(job.frame.sample.get())),
                                job.frame.width, job.frame.height, boxes);
                            runtime->record("Person crop", job.channel, elapsed(t), waited,
                                            crops[j].size());
                            for (auto box : job.boxes)
                                people[j].push_back({box, {}});
                            if (reid)
                                all.insert(all.end(), crops[j].begin(), crops[j].end());
                        } catch (const std::exception &e) {
                            failed[j] = e.what();
                        }
                    }
                    if (reid && !all.empty()) {
                        try {
                            auto features = reid->encode_many(all);
                            size_t offset = 0;
                            for (size_t j = 0; j < batch.size(); ++j) {
                                if (!failed[j].empty())
                                    continue;
                                for (auto &person : people[j])
                                    person.feature = features[offset++];
                                // Shares of the batched call, by images.
                                const double share = double(crops[j].size()) / all.size();
                                if (crops[j].empty())
                                    continue;
                                runtime->record("ReID preprocess", batch[j].channel,
                                                reid->prepare_ms * share, 0, crops[j].size());
                                runtime->record("ReID inference + transfer", batch[j].channel,
                                                reid->inference_ms * share, 0, crops[j].size());
                            }
                        } catch (const std::exception &e) {
                            for (auto &f : failed)
                                if (f.empty())
                                    f = e.what();
                        }
                    }
                }
                for (size_t j = 0; j < batch.size(); ++j) {
                    auto &job = batch[j];
                    auto &images = crops[j];
                    auto &detections = people[j];
                    try {
                        if (!failed[j].empty())
                            throw std::runtime_error(failed[j]);
                        auto &tracker = trackers[job.channel];
                        if (sessions[job.channel] != job.frame.recording_session) {
                            tracker.reset();
                            sessions[job.channel] = job.frame.recording_session;
                        }
                        if (!tracker)
                            tracker = std::make_unique<PersonTracker>();
                        auto t = Clock::now();
                        auto identities = tracker->update(job.frame.wall_time_ms, detections);
                        runtime->record(
                            "Tracking association", job.channel, elapsed(t),
                            std::chrono::duration<double, std::milli>(t - job.frame.received)
                                .count(),
                            detections.size());
                        PersonTrackFrame frame;
                        frame.generation = job.frame.generation;
                        frame.width = job.frame.width;
                        frame.height = job.frame.height;
                        frame.captured = job.frame.received;
                        RecordedAnalysis recorded{job.channel,
                                                  job.frame.recording_session,
                                                  run_id,
                                                  job.frame.wall_time_ms,
                                                  unsigned(std::min(2000., 2000. / runtime->fps())),
                                                  {}};
                        for (size_t i = 0; i < detections.size(); ++i) {
                            if (reid && !identities[i].confirmed)
                                continue;
                            auto b = detections[i].box;
                            b.x /= job.frame.width;
                            b.w /= job.frame.width;
                            b.y /= job.frame.height;
                            b.h /= job.frame.height;
                            recorded.objects.push_back(
                                {b, reid ? identities[i].person : 0,
                                 reid ? identities[i].track : 0});
                        }
                        catalog.enqueue_analysis(std::move(recorded));
                        std::lock_guard<std::mutex> lock(mutex);
                        for (size_t i = 0; i < detections.size(); ++i) {
                            const auto &identity = identities[i];
                            auto box = detections[i].box;
                            if (reid && identity.confirmed)
                                frame.objects.push_back({box, identity.person});
                            if ((reid && (!identity.confirmed || box.score < .5f)) ||
                                box.h < job.frame.height * options.min_person_height)
                                continue;
                            SearchObservation o;
                            o.channel = job.channel;
                            o.session = job.frame.recording_session;
                            o.time_ms = job.frame.wall_time_ms;
                            o.box = box;
                            o.model = key;
                            o.reid = detections[i].feature;
                            std::string id = run_id + "/" + o.channel + "/" + o.session + "/";
                            if (reid) {
                                o.person_id = id + std::to_string(identity.person);
                                o.track_id = id + std::to_string(identity.track);
                                id = o.person_id;
                            } else
                                id += std::to_string(i);
                            if (!pending.count(id) && pending.size() >= options.queue_limit) {
                                ++status.dropped;
                                runtime->queue("Embedding queue", "", pending.size(), 1);
                                continue;
                            }
                            auto queued = pending.count(id) ? pending[id].queued : Clock::now();
                            pending[id] = {std::move(o), std::move(images[i]), queued,
                                           Clock::now()};
                        }
                        if (reid)
                            track_frames[job.channel] = std::move(frame);
                        status.pending = pending.size();
                        runtime->queue("Embedding queue", "", pending.size());
                        cv.notify_all();
                    } catch (const std::exception &e) {
                        std::lock_guard<std::mutex> l(mutex);
                        status.error = e.what();
                        status.dropped += job.boxes.size();
                    }
                }
            }
        } catch (const std::exception &e) {
            fail(e);
        }
    }
    // Independent device jobs: NPU never owns the GPU gate, and finishing one
    // device immediately permits another job on that device (no paired barrier).
    void run_mobileclip(Embedder &model, const std::string &npu_key) {
        struct Completion {
            std::vector<std::pair<std::string, Pending>> items;
            std::vector<std::array<float, 768>> embeddings;
            std::string error;
            Clock::time_point finished{};
            double ms{};
        };
        const bool enabled[] = {options.mobileclip_backend != "npu",
                                options.mobileclip_backend != "gpu"};
        auto client = [&](unsigned device) -> MobileClipClient & {
            return device == 1 && model.combined ? *model.secondary : *model.mobileclip;
        };
        std::array<std::future<Completion>, 2> running;
        std::array<Clock::time_point, 2> last_end{};
        std::array<double, 2> last_ms{};
        std::set<std::string> in_flight;
        auto ready = [](auto &future) {
            return future.valid() &&
                   future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        };
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping)
                    return; // Futures drain before the model/worker processes are destroyed.
            }
            for (unsigned device = 0; device < 2; ++device) {
                if (!ready(running[device]))
                    continue;
                auto done = running[device].get();
                last_end[device] = done.finished;
                last_ms[device] = done.ms;
                for (size_t i = 0; i < done.items.size(); ++i) {
                    auto &[id, item] = done.items[i];
                    bool stored = false;
                    std::string error = done.error;
                    if (error.empty()) {
                        try {
                            item.observation.embedding = done.embeddings.at(i);
                            item.observation.model = device == 1 && model.combined ? npu_key : key;
                            auto start = Clock::now();
                            item.observation.png = png_encode(item.image);
                            stored = index.store(item.observation);
                            runtime->record("PNG + DB save", item.observation.channel,
                                            elapsed(start));
                        } catch (const std::exception &e) {
                            error = e.what();
                        }
                    }
                    std::lock_guard<std::mutex> lock(mutex);
                    in_flight.erase(id);
                    if (stored) {
                        ++status.stored;
                        last_saved[id] = item.observation.time_ms;
                    } else
                        ++status.dropped;
                    if (!error.empty())
                        status.error = error;
                    status.inference_ms = done.ms;
                }
                runtime->queue(device ? "Embedding NPU in flight" : "Embedding GPU in flight", "",
                               0);
            }
            Request request;
            bool query_pending;
            {
                std::lock_guard<std::mutex> lock(mutex);
                query_pending = query.id != 0;
                if (query_pending && !running[0].valid() && !running[1].valid()) {
                    request = std::move(query);
                    query = {};
                }
            }
            // Drain at most one already-admitted batch per device before a user query.
            if (request.id) {
                try {
                    const auto start = Clock::now();
                    std::array<float, 768> gpu{}, npu{};
                    if (request.image.rgb.empty()) {
                        // CPU text encoder does not consume the GPU gate.
                        gpu = text_query(request, [&](const std::string &text) {
                            return model.mobileclip->text(text);
                        });
                        npu = gpu;
                    } else {
                        const std::vector<SearchImage> input{request.image};
                        std::future<std::vector<std::array<float, 768>>> npu_query;
                        if (enabled[1])
                            npu_query = std::async(std::launch::async, [&] {
                                return model.mobile_images(client(1), input, true);
                            });
                        if (enabled[0]) {
                            auto lease = runtime->acquire(AiRuntime::Query);
                            gpu = model.mobile_images(client(0), input, false).front();
                        }
                        if (npu_query.valid())
                            npu = npu_query.get().front();
                    }
                    runtime->record(request.image.rgb.empty() ? "Text query" : "Image query", "",
                                    elapsed(start));
                    auto search_start = Clock::now();
                    auto hits = index.search(enabled[0] ? gpu : npu, key);
                    if (model.combined) {
                        auto more = index.search(npu, npu_key);
                        hits.insert(hits.end(), std::make_move_iterator(more.begin()),
                                    std::make_move_iterator(more.end()));
                    }
                    auto groups = group_search_results(std::move(hits));
                    runtime->record("Vector search", "", elapsed(search_start));
                    std::lock_guard<std::mutex> lock(mutex);
                    if (request.id == serial) {
                        result = {request.id, {}, std::move(groups)};
                        status.error.clear();
                    }
                } catch (const std::exception &e) {
                    std::lock_guard<std::mutex> lock(mutex);
                    status.error = e.what();
                    if (request.id == serial)
                        result = {request.id, e.what(), {}};
                }
            }
            const auto requested = runtime->interval_ms();
            runtime->effective_interval(requested);
            const auto budgets = runtime->mobileclip_budget();
            // Prefer free NPU capacity, leaving GPU time available for tracking.
            for (unsigned device : {1u, 0u}) {
                if (query_pending || !enabled[device] || running[device].valid())
                    continue;
                const unsigned budget = device ? budgets.second : budgets.first;
                const double idle_ms = last_ms[device] * (100. - budget) / budget;
                if (std::chrono::duration<double, std::milli>(Clock::now() - last_end[device])
                        .count() < idle_ms)
                    continue;
                if (!device && !runtime->embedding_ready(true))
                    continue;
                std::vector<std::pair<std::string, Pending>> items;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping || query.id)
                        break;
                    std::vector<std::pair<std::string, Clock::time_point>> eligible;
                    for (const auto &[id, candidate] : pending) {
                        const auto saved = last_saved.find(id);
                        if (Clock::now() - candidate.updated <= std::chrono::seconds(30) &&
                            !in_flight.count(id) &&
                            (saved == last_saved.end() ||
                             candidate.observation.time_ms - saved->second >= requested))
                            eligible.emplace_back(id, candidate.queued);
                    }
                    std::sort(eligible.begin(), eligible.end(),
                              [](const auto &a, const auto &b) { return a.second < b.second; });
                    const unsigned capacity = device ? 1 : model.gpu_capacity;
                    for (const auto &[id, queued] : eligible) {
                        if (items.size() == capacity)
                            break;
                        in_flight.insert(id);
                        items.emplace_back(id, std::move(pending.at(id)));
                        pending.erase(id);
                    }
                }
                if (items.empty())
                    continue;
                runtime->queue(device ? "Embedding NPU in flight" : "Embedding GPU in flight", "",
                               items.size());
                running[device] =
                    std::async(std::launch::async, [&, device, items = std::move(items)]() mutable {
                        Completion done;
                        done.items = std::move(items);
                        auto start = Clock::now();
                        try {
                            std::vector<SearchImage> images;
                            for (const auto &item : done.items)
                                images.push_back(item.second.image);
                            if (device) {
                                start = Clock::now();
                                done.embeddings = model.mobile_images(client(device), images, true);
                            } else {
                                auto lease = runtime->acquire(AiRuntime::MobileClip);
                                start = Clock::now();
                                done.embeddings =
                                    model.mobile_images(client(device), images, false);
                            }
                            done.ms = elapsed(start);
                            runtime->record("Image embedding", "", done.ms,
                                            std::chrono::duration<double, std::milli>(
                                                start - done.items.front().second.queued)
                                                .count(),
                                            done.items.size());
                        } catch (const std::exception &e) {
                            done.error = e.what();
                            done.ms = elapsed(start);
                        }
                        done.finished = Clock::now();
                        cv.notify_all();
                        return done;
                    });
            }
            {
                std::unique_lock<std::mutex> lock(mutex);
                const auto now = Clock::now();
                for (auto it = pending.begin(); it != pending.end();) {
                    const auto saved = last_saved.find(it->first);
                    const auto age = now - it->second.updated;
                    const bool redundant =
                        age > std::chrono::seconds(5) && saved != last_saved.end() &&
                        it->second.observation.time_ms - saved->second < requested;
                    if (age > std::chrono::seconds(30) || redundant) {
                        it = pending.erase(it);
                        if (!redundant) {
                            ++status.dropped;
                            runtime->queue("Embedding queue", "", pending.size(), 1);
                        }
                    } else
                        ++it;
                }
                for (auto it = last_saved.begin(); it != last_saved.end();)
                    if (!in_flight.count(it->first) &&
                        g_get_real_time() / 1000 - it->second > 120000)
                        it = last_saved.erase(it);
                    else
                        ++it;
                status.pending = pending.size();
                runtime->queue("Embedding queue", "", pending.size());
                cv.wait_for(lock, std::chrono::milliseconds(10));
            }
        }
    }
    void run() {
        try {
            if (options.embedding_model == "mobileclip2-s2" && options.mobileclip_backend == "npu")
                options.embedding_batch = 1;
            hip_check(hipSetDevice(options.device), "Search GPU");
            Embedder model(options);
            std::string npu_key;
            if (model.combined) {
                auto backend = options;
                backend.mobileclip_backend = "gpu";
                key = fingerprint(backend);
                backend.mobileclip_backend = "npu";
                npu_key = fingerprint(backend);
                // One concurrent batch contains the GPU capacity plus one NPU image.
                ++options.embedding_batch;
            } else
                key = fingerprint(options);
            SearchImage warm{1, 1, {0, 0, 0}};
            // Warm the fixed image execution profile before accepting cameras.
            model.encode_batch(std::vector<SearchImage>(options.embedding_batch, warm), options);
            model.encode(nullptr, "person", options.image_size);
            auto warm_started = Clock::now();
            model.encode_batch(std::vector<SearchImage>(options.embedding_batch, warm), options);
            double image_ms = elapsed(warm_started) / options.embedding_batch;
            model.metrics = runtime;
            tracking_worker = std::thread([this] { track(); });
            if (model.mobileclip) {
                run_mobileclip(model, npu_key);
                return;
            }
            for (;;) {
                Request request;
                std::vector<Pending> batch;
                std::vector<std::string> ids;
                {
                    std::unique_lock<std::mutex> l(mutex);
                    cv.wait_for(l, std::chrono::milliseconds(100),
                                [&] { return stopping || query.id; });
                    if (stopping)
                        break;
                    if (query.id) {
                        request = std::move(query);
                        query = {};
                    } else {
                        auto now = Clock::now();
                        const unsigned requested = runtime->interval_ms();
                        unsigned active = 0;
                        for (auto it = pending.begin(); it != pending.end();) {
                            const auto age = now - it->second.updated;
                            const auto saved = last_saved.find(it->first);
                            const bool redundant =
                                age > std::chrono::seconds(5) && saved != last_saved.end() &&
                                it->second.observation.time_ms - saved->second < requested;
                            if (age > std::chrono::seconds(30) || redundant) {
                                it = pending.erase(it);
                                if (!redundant) {
                                    ++status.dropped;
                                    runtime->queue("Embedding queue", "", pending.size(), 1);
                                }
                            } else {
                                if (age <= std::chrono::seconds(5) || saved == last_saved.end())
                                    ++active;
                                ++it;
                            }
                        }
                        const double required = active * image_ms / .2;
                        const unsigned effective = std::max(
                            requested,
                            unsigned(std::clamp(std::ceil(required / 100.) * 100., 100., 60000.)));
                        runtime->effective_interval(effective);
                        std::vector<std::pair<std::string, Clock::time_point>> eligible;
                        for (auto &[id, p] : pending)
                            if (p.observation.time_ms - last_saved[id] >= effective ||
                                (now - p.updated > std::chrono::seconds(5) &&
                                 p.observation.time_ms - last_saved[id] >= requested))
                                eligible.emplace_back(id, p.queued);
                        std::sort(eligible.begin(), eligible.end(),
                                  [](auto &a, auto &b) { return a.second < b.second; });
                        if (runtime->embedding_ready() &&
                            (eligible.size() >= options.embedding_batch ||
                             (!eligible.empty() &&
                              now - eligible.front().second >=
                                  std::chrono::milliseconds(std::min(1000u, requested)))))
                            for (auto &e : eligible) {
                                if (batch.size() >= options.embedding_batch)
                                    break;
                                ids.push_back(e.first);
                                batch.push_back(std::move(pending.at(e.first)));
                                pending.erase(e.first);
                            }
                        status.pending = pending.size();
                        runtime->queue("Embedding queue", "", pending.size());
                        for (auto it = last_saved.begin(); it != last_saved.end();)
                            if (g_get_real_time() / 1000 - it->second > 120000)
                                it = last_saved.erase(it);
                            else
                                ++it;
                    }
                }
                if (!request.id && batch.empty())
                    continue;
                try {
                    if (request.id) {
                        std::array<float, 768> embedding, npu_embedding;
                        auto wait = Clock::now();
                        {
                            auto lease = runtime->acquire(AiRuntime::Query);
                            auto t = Clock::now();
                            if (model.combined && request.image.rgb.empty()) {
                                // Text is the same CPU encoder for both image backends.
                                embedding = npu_embedding =
                                    text_query(request, [&](const std::string &text) {
                                        return model.combined_query(nullptr, text).first;
                                    });
                            } else if (model.combined) {
                                auto queries = model.combined_query(&request.image, {});
                                embedding = queries.first;
                                npu_embedding = queries.second;
                            } else
                                embedding =
                                    request.image.rgb.empty()
                                        ? text_query(request,
                                                     [&](const std::string &text) {
                                                         return model.encode(nullptr, text,
                                                                             options.image_size);
                                                     })
                                        : model.encode_batch({request.image}, options).front();
                            runtime->record(
                                request.image.rgb.empty() ? "Text query" : "Image query", "",
                                elapsed(t),
                                std::chrono::duration<double, std::milli>(t - wait).count());
                        }
                        auto t = Clock::now();
                        auto hits = index.search(embedding, key);
                        if (model.combined) {
                            auto npu_hits = index.search(npu_embedding, npu_key);
                            hits.insert(hits.end(), std::make_move_iterator(npu_hits.begin()),
                                        std::make_move_iterator(npu_hits.end()));
                        }
                        auto groups = group_search_results(std::move(hits));
                        runtime->record("Vector search", "", elapsed(t));
                        std::lock_guard<std::mutex> l(mutex);
                        if (request.id == serial) {
                            result = {request.id, {}, std::move(groups)};
                            status.error.clear();
                        }
                    } else {
                        std::vector<SearchImage> images;
                        for (auto &p : batch)
                            images.push_back(std::move(p.image));
                        std::vector<std::array<float, 768>> embeddings;
                        double ms;
                        {
                            auto lease = runtime->acquire(AiRuntime::Embedding);
                            auto t = Clock::now();
                            embeddings = model.encode_batch(images, options);
                            ms = elapsed(t);
                            runtime->record(
                                "Image embedding", "", ms,
                                std::chrono::duration<double, std::milli>(t - batch.front().queued)
                                    .count(),
                                batch.size());
                        }
                        image_ms =
                            image_ms ? image_ms * .8 + ms / batch.size() * .2 : ms / batch.size();
                        for (size_t i = 0; i < batch.size(); ++i) {
                            auto &p = batch[i];
                            p.observation.embedding = embeddings[i];
                            if (model.combined)
                                p.observation.model = i < model.last_gpu_count ? key : npu_key;
                            auto t = Clock::now();
                            p.observation.png = png_encode(images[i]);
                            bool stored = index.store(p.observation);
                            runtime->record("PNG + DB save", p.observation.channel, elapsed(t));
                            std::lock_guard<std::mutex> l(mutex);
                            if (stored) {
                                ++status.stored;
                                last_saved[ids[i]] = p.observation.time_ms;
                            } else
                                ++status.dropped;
                            status.inference_ms = ms;
                        }
                    }
                } catch (const std::exception &e) {
                    std::lock_guard<std::mutex> l(mutex);
                    status.error = e.what();
                    if (request.id && request.id == serial)
                        result = {request.id, e.what(), {}};
                    else
                        status.dropped += batch.size();
                }
            }
        } catch (const std::exception &e) {
            fail(e);
        }
    }
};
SearchService::SearchService(SearchIndex &i, GstVaDisplay *d, SearchOptions o,
                             std::shared_ptr<AiRuntime> r)
    : impl_(std::make_unique<Impl>(i, d, std::move(o), std::move(r))) {}
SearchService::~SearchService() = default;
void SearchService::shutdown() { impl_->stop(); }
void SearchService::wait_initialized() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->cv.wait(lock, [this] { return impl_->status.state != "loading"; });
}
void SearchService::submit(const Frame &f, const std::string &channel,
                           const std::vector<Detection> &boxes) {
    auto &p = *impl_;
    std::lock_guard<std::mutex> l(p.mutex);
    if (p.stopping || p.status.state != "ready" || !f.wall_time_ms || f.recording_session.empty())
        return;
    // Detection admission already applies the per-camera sampling frequency.
    std::vector<Detection> people;
    for (auto b : boxes)
        if (b.class_id == 0 && b.w > 0 && b.h > 0 && b.h >= f.height * p.options.min_person_height)
            people.push_back(b);
    if (people.size() > 64)
        people.resize(64);
    auto old =
        std::find_if(p.jobs.begin(), p.jobs.end(), [&](auto &j) { return j.channel == channel; });
    if (old != p.jobs.end()) {
        p.status.dropped += old->boxes.size();
        *old = {f, channel, std::move(people)};
        p.runtime->queue("Tracking queue", "", p.jobs.size(), 1);
    } else if (p.jobs.size() < 16)
        p.jobs.push_back({f, channel, std::move(people)});
    else {
        p.status.dropped += people.size();
        p.runtime->queue("Tracking queue", "", p.jobs.size(), 1);
    }
    p.runtime->queue("Tracking queue", "", p.jobs.size());
    p.cv.notify_all();
}

uint64_t SearchService::text(std::string text, std::vector<std::string> negatives,
                             std::vector<std::string> alternatives) {
    auto &p = *impl_;
    std::lock_guard<std::mutex> l(p.mutex);
    auto id = ++p.serial;
    p.result = {};
    if (text.empty() || text.size() > 16384 || p.status.state == "error" || p.stopping) {
        p.result = {
            id, p.status.error.empty() ? "Search unavailable or empty query" : p.status.error, {}};
        return id;
    }
    p.query = {id, std::move(text), {}, std::move(negatives), std::move(alternatives)};
    p.cv.notify_one();
    return id;
}
uint64_t SearchService::image(SearchImage im) {
    auto &p = *impl_;
    std::lock_guard<std::mutex> l(p.mutex);
    auto id = ++p.serial;
    p.result = {};
    if (!im.width || !im.height || im.width > p.options.image_size ||
        im.height > p.options.image_size || im.rgb.size() != size_t(im.width) * im.height * 3) {
        p.result = {id, "Invalid search image", {}};
        return id;
    }
    if (p.status.state == "error" || p.stopping) {
        p.result = {id, p.status.error.empty() ? "Search unavailable" : p.status.error, {}};
        return id;
    }
    p.query = {id, {}, std::move(im)};
    p.cv.notify_one();
    return id;
}
void SearchService::cancel() {
    auto &p = *impl_;
    std::lock_guard<std::mutex> l(p.mutex);
    ++p.serial;
    p.query = {};
    p.result = {};
}
SearchResult SearchService::result() const {
    std::lock_guard<std::mutex> l(impl_->mutex);
    return impl_->result;
}
SearchStatus SearchService::status() const {
    std::lock_guard<std::mutex> l(impl_->mutex);
    return impl_->status;
}
PersonTrackFrame SearchService::tracks(const std::string &channel) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->track_frames.find(channel);
    return it == impl_->track_frames.end() ? PersonTrackFrame{} : it->second;
}
} // namespace amdva::nvr
