#include "backend/mobileclip_ipc.hpp"
#include <algorithm>
#include <csignal>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <sys/prctl.h>
#include <vector>
using namespace amdva::nvr::mc2ipc;
namespace {
// ABI from embeddinggemma2-rocm/include/mobileclip2/mobileclip2.h.
struct Image {
    const uint8_t *data;
    size_t width, height, row_stride;
};
template <class F> F symbol(void *lib, const char *name) {
    auto f = reinterpret_cast<F>(dlsym(lib, name));
    if (!f)
        throw std::runtime_error(std::string("Missing MobileCLIP symbol: ") + name);
    return f;
}
void reply(const std::vector<float> &values, const std::string &error = {}) {
    write(3, error.empty() ? 0 : 1);
    write(3, error.empty() ? values.size() * sizeof(float) : error.size());
    if (!error.empty())
        transfer(3, const_cast<char *>(error.data()), error.size(), true);
    else if (!values.empty())
        transfer(3, const_cast<float *>(values.data()), values.size() * sizeof(float), true);
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        return 2;
    void *lib = nullptr, *ctx = nullptr;
    void (*destroy)(void *) = nullptr;
    try {
        lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            throw std::runtime_error(dlerror());
        auto create =
            symbol<int (*)(const char *, const char *, void **, char *, size_t)>(lib, "mc2_create");
        destroy = symbol<void (*)(void *)>(lib, "mc2_destroy");
        auto prepare = symbol<int (*)(void *, char *, size_t)>(lib, "mc2_prepare");
        auto images =
            symbol<int (*)(void *, const Image *, size_t, float *, size_t, char *, size_t)>(
                lib, "mc2_encode_images");
        auto texts =
            symbol<int (*)(void *, const char *const *, size_t, float *, size_t, char *, size_t)>(
                lib, "mc2_encode_texts");
        char error[4096]{};
        if (create(argv[2], argv[3], &ctx, error, sizeof(error)) ||
            prepare(ctx, error, sizeof(error)))
            throw std::runtime_error(error);
        const bool npu = std::string(argv[3]) == "npu";
        reply({});
        for (;;) {
            const auto op = read(3, -1);
            if (!op)
                break;
            if (op == 1) {
                const auto size = read(3);
                if (!size || size > 65536)
                    throw std::runtime_error("Invalid text length");
                std::string text(size, '\0');
                transfer(3, text.data(), size, false);
                const char *p = text.c_str();
                std::vector<float> out(512);
                if (texts(ctx, &p, 1, out.data(), out.size(), error, sizeof(error)))
                    reply({}, error);
                else
                    reply(out);
            } else if (op == 2) {
                const auto count = read(3);
                if (!count || count > 4)
                    throw std::runtime_error("Invalid image batch");
                std::vector<std::vector<uint8_t>> pixels(count);
                std::vector<Image> input;
                for (unsigned i = 0; i < count; ++i) {
                    const auto w = read(3), h = read(3);
                    if (!w || !h || w > 1024 || h > 1024)
                        throw std::runtime_error("Invalid image size");
                    pixels[i].resize(size_t(w) * h * 3);
                    transfer(3, pixels[i].data(), pixels[i].size(), false);
                    input.push_back({pixels[i].data(), w, h, size_t(w) * 3});
                }
                const auto physical = npu ? count : (count == 1 ? 1u : 4u);
                while (input.size() < physical)
                    input.push_back(input.back());
                std::vector<float> out(physical * 512);
                int rc = 0;
                if (npu) {
                    for (unsigned i = 0; i < count && !rc; ++i)
                        rc = images(ctx, &input[i], 1, out.data() + i * 512, 512, error,
                                    sizeof(error));
                } else
                    rc = images(ctx, input.data(), physical, out.data(), out.size(), error,
                                sizeof(error));
                if (rc)
                    reply({}, error);
                else {
                    out.resize(count * 512);
                    reply(out);
                }
            } else
                throw std::runtime_error("Invalid MobileCLIP operation");
        }
    } catch (const std::exception &e) {
        std::cerr << "MobileCLIP worker: " << e.what() << '\n';
        try {
            reply({}, e.what());
        } catch (...) {
        }
        if (ctx && destroy)
            destroy(ctx);
        if (lib)
            dlclose(lib);
        return 1;
    }
    if (ctx)
        destroy(ctx);
    if (lib)
        dlclose(lib);
    return 0;
}
