#include "amdva/streaming/search.hpp"
#include "../model_json.hpp"
#include "amdva/streaming/controller.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <gst/va/gstva.h>
#include <png.h>
#include <sstream>
extern "C" void amdva_audit_search_begin(const void *, size_t, int) __attribute__((weak));
extern "C" void amdva_audit_search_end() __attribute__((weak));
extern "C" void amdva_audit_embedding_scope(bool) __attribute__((weak));
namespace amdva::streaming {
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
        return std::tie(a.channel, a.time_ms, a.id) < std::tie(b.channel, b.time_ms, b.id);
    });
    std::vector<SearchGroup> groups;
    for (auto &o : hits) {
        if (groups.empty() || groups.back().channel != o.channel ||
            o.time_ms - groups.back().to_ms > 10000 || o.time_ms - groups.back().from_ms > 60000)
            groups.push_back({o.channel, o.camera_name, o.time_ms, o.time_ms, o.similarity, {}});
        auto &g = groups.back();
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
        converted = va.create_bgra(1920, 1080);
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
    if (width > 1920 || height > 1080)
        throw std::invalid_argument("Unsupported search source size");
    b = search_crop(b, width, height);
    auto &p = *impl_;
    p.va.scale(surface, p.converted, width, height, width, height);
    va_check(vaSyncSurface(p.va.display(), p.converted), "Search surface sync");
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
    return image;
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
    std::array<float, 768> encode(const SearchImage *im, const std::string &text, unsigned size) {
        EmbeddingScope scope;
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
    const auto root = std::filesystem::path(o.bundle);
    Parser index((root / "index.json").string());
    std::string input = "rgb-square-black-v1/" + std::to_string(o.image_size) + "/70/";
    auto append = [&](const std::filesystem::path &path) {
        std::ifstream f(path);
        if (!f)
            throw std::runtime_error("Cannot read embedding manifest: " + path.string());
        std::ostringstream s;
        s << f.rdbuf();
        input += s.str();
    };
    append(root / "index.json");
    auto *profiles = required_array(index.object(), "profiles");
    for (unsigned i = 0; i < json_array_get_length(profiles); ++i) {
        auto *profile = json_array_get_object_element(profiles, i);
        append(root / required_string(profile, "path") / "bundle.json");
    }
    // Bundle manifests identify the model revision, compiled profiles and weight hashes.
    auto *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, input.c_str(), input.size());
    std::string result(hash);
    g_free(hash);
    return result;
}
} // namespace
struct SearchService::Impl {
    Catalog &catalog;
    GstVaDisplay *display;
    SearchOptions options;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool stopping{};
    struct Job {
        Frame frame;
        std::string channel;
        std::vector<Detection> boxes;
    };
    std::deque<Job> jobs;
    std::map<std::string, Clock::time_point> admitted;
    struct Request {
        uint64_t id{};
        std::string text;
        SearchImage image;
    };
    Request query;
    uint64_t serial{};
    SearchResult result;
    SearchStatus status;
    Impl(Catalog &c, GstVaDisplay *d, SearchOptions o)
        : catalog(c), display(d), options(std::move(o)) {
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
            status.pending = 0;
            query = {};
            cv.notify_all();
        }
        if (worker.joinable())
            worker.join();
    }
    void run() {
        try {
            hip_check(hipSetDevice(options.device), "Search GPU");
            Embedder model(options);
            SearchCropper cropper(display, options.image_size, options.device);
            auto key = fingerprint(options);
            // Validate both profiles before accepting collection work.
            SearchImage warm{1, 1, {0, 0, 0}};
            model.encode(&warm, "", options.image_size);
            model.encode(nullptr, "person", options.image_size);
            {
                std::lock_guard<std::mutex> l(mutex);
                status.state = "ready";
                cv.notify_all();
            }
            while (true) {
                Job job;
                Request request;
                {
                    std::unique_lock<std::mutex> l(mutex);
                    cv.wait(l, [&] { return stopping || query.id || !jobs.empty(); });
                    if (stopping)
                        break;
                    if (query.id) {
                        request = std::move(query);
                        query = {};
                    } else {
                        job = std::move(jobs.front());
                        jobs.pop_front();
                    }
                }
                try {
                    auto started = Clock::now();
                    if (request.id) {
                        auto embedding =
                            model.encode(request.image.rgb.empty() ? nullptr : &request.image,
                                         request.text, options.image_size);
                        auto groups = group_search_results(catalog.search(embedding, key));
                        std::lock_guard<std::mutex> l(mutex);
                        if (request.id == serial)
                            result = {request.id, {}, std::move(groups)};
                    } else {
                        // One crop per scheduling turn; a pending user query runs before the next
                        // crop.
                        auto box = job.boxes.front();
                        job.boxes.erase(job.boxes.begin());
                        auto b = search_crop(box, job.frame.width, job.frame.height, .1f);
                        auto im = cropper.crop(gst_va_buffer_get_surface(
                                                   gst_sample_get_buffer(job.frame.sample.get())),
                                               job.frame.width, job.frame.height, b);
                        SearchObservation o;
                        o.channel = job.channel;
                        o.session = job.frame.recording_session;
                        o.time_ms = job.frame.wall_time_ms;
                        o.box = b;
                        o.model = key;
                        o.embedding = model.encode(&im, "", options.image_size);
                        o.png = png_encode(im);
                        bool stored = catalog.store_observation(o);
                        std::lock_guard<std::mutex> l(mutex);
                        if (stored)
                            status.stored++;
                        else
                            status.dropped++;
                        if (status.pending)
                            status.pending--;
                        if (!stopping && !job.boxes.empty())
                            jobs.push_back(std::move(job));
                    }
                    std::lock_guard<std::mutex> l(mutex);
                    status.error.clear();
                    status.inference_ms =
                        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                } catch (const std::exception &e) {
                    std::lock_guard<std::mutex> l(mutex);
                    status.error = e.what();
                    if (request.id) {
                        if (request.id == serial)
                            result = {request.id, e.what(), {}};
                    } else {
                        auto n = job.boxes.size() + 1;
                        status.dropped += n;
                        status.pending = status.pending > n ? status.pending - n : 0;
                    }
                }
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> l(mutex);
            status.state = "error";
            cv.notify_all();
            status.error = e.what();
            jobs.clear();
            status.pending = 0;
            if (query.id)
                result = {query.id, e.what(), {}};
        }
    }
};
SearchService::SearchService(Catalog &c, GstVaDisplay *d, SearchOptions o)
    : impl_(std::make_unique<Impl>(c, d, std::move(o))) {}
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
    auto now = Clock::now();
    if (now - p.admitted[channel] < std::chrono::milliseconds(p.options.interval_ms))
        return;
    std::vector<Detection> people;
    for (auto b : boxes)
        if (b.class_id == 0 && b.w > 0 && b.h > 0)
            people.push_back(b);
    if (people.empty())
        return;
    p.admitted[channel] = now;
    if (p.jobs.size() >= 3 || p.status.pending + people.size() > p.options.queue_limit) {
        p.status.dropped += people.size();
        return;
    }
    p.status.pending += people.size();
    p.jobs.push_back({f, channel, std::move(people)});
    p.cv.notify_one();
}
uint64_t SearchService::text(std::string text) {
    auto &p = *impl_;
    std::lock_guard<std::mutex> l(p.mutex);
    auto id = ++p.serial;
    p.result = {};
    if (text.empty() || text.size() > 16384 || p.status.state == "error" || p.stopping) {
        p.result = {
            id, p.status.error.empty() ? "Search unavailable or empty query" : p.status.error, {}};
        return id;
    }
    p.query = {id, std::move(text), {}};
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
} // namespace amdva::streaming
