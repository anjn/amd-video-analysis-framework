#include "renderer.hpp"
#include "amdva/runtime.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

// Extension typedefs require the core GL types above.
#include <GLES2/gl2ext.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay_drm.h>
#include <gst/va/gstvapool.h>
#include <gst/va/gstvautils.h>
#include <iomanip>
#include <sstream>
#include <unistd.h>
#include <va/va_vpp.h>
namespace amdva::dashboard {
namespace {
using Value = std::optional<double>;
struct Color {
    float r, g, b, a;
};
Color color(unsigned rgb, float a = 1) {
    return {float((rgb >> 16) & 255) / 255, float((rgb >> 8) & 255) / 255, float(rgb & 255) / 255,
            a};
}
const auto white = color(0xedf3fc), muted = color(0xbdcce0), faint = color(0x293b4d),
           cyan = color(0x61dcf3), purple = color(0xad9aff), amber = color(0xf6bd69),
           green = color(0x60dcb0);
std::string fmt(Value n, int places = 1) {
    if (!n || !std::isfinite(*n))
        return "N/A";
    std::ostringstream s;
    s << std::fixed << std::setprecision(places) << *n;
    return s.str();
}
bool nonnegative(Value n) { return n && std::isfinite(*n) && *n >= 0; }
bool ratio(Value n, Value d) {
    return nonnegative(n) && d && *d > 0 && std::isfinite(*d) && *n <= *d;
}
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
GLuint shader(GLenum type, const char *text) {
    auto s = glCreateShader(type);
    glShaderSource(s, 1, &text, nullptr);
    glCompileShader(s);
    GLint ok{};
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        glDeleteShader(s);
        throw std::runtime_error(log);
    }
    return s;
}
struct Vertex {
    float x, y, u, v, r, g, b, a;
};
struct Glyph {
    float u, v, w, h, left, top, advance;
};
} // namespace
struct Renderer::Impl {
    GstVaDisplay *gst_display{};
    std::unique_ptr<VaSession> va;
    EGLDisplay egl{EGL_NO_DISPLAY};
    EGLContext context{EGL_NO_CONTEXT};
    EGLImageKHR image{EGL_NO_IMAGE_KHR};
    GLuint texture{}, fbo{}, program{}, vbo{}, vao{}, atlas{};
    PFNEGLDESTROYIMAGEKHRPROC destroy_image{};
    VASurfaceID bgra{VA_INVALID_SURFACE};
    VAConfigID config{VA_INVALID_ID};
    VAContextID vpp{VA_INVALID_ID};
    GstBufferPool *pool{};
    std::vector<Vertex> vertices;
    std::array<Glyph, 128> glyphs{};
    double power_max{};
    float ascender{};
    std::string renderer;
    ~Impl() {
        if (pool) {
            gst_buffer_pool_set_active(pool, FALSE);
            gst_object_unref(pool);
        }
        if (context != EGL_NO_CONTEXT && egl != EGL_NO_DISPLAY) {
            eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
            if (vbo)
                glDeleteBuffers(1, &vbo);
            if (vao)
                glDeleteVertexArrays(1, &vao);
            if (program)
                glDeleteProgram(program);
            if (atlas)
                glDeleteTextures(1, &atlas);
            if (fbo)
                glDeleteFramebuffers(1, &fbo);
            if (texture)
                glDeleteTextures(1, &texture);
            if (image != EGL_NO_IMAGE_KHR && destroy_image)
                destroy_image(egl, image);
            eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(egl, context);
        }
        if (egl != EGL_NO_DISPLAY)
            eglTerminate(egl);
        if (va) {
            if (vpp != VA_INVALID_ID)
                vaDestroyContext(va->display(), vpp);
            if (config != VA_INVALID_ID)
                vaDestroyConfig(va->display(), config);
            va->destroy_surface(bgra);
            va.reset();
        }
        if (gst_display)
            gst_object_unref(gst_display);
    }
    void init(const std::string &node, const std::string &font, unsigned fps, double max_w) {
        power_max = max_w;
        gst_display = gst_va_display_drm_new_from_path(node.c_str());
        require(gst_display, "Cannot open VA render device");
        va = std::make_unique<VaSession>(
            static_cast<VADisplay>(gst_va_display_get_va_dpy(gst_display)));
        // Select the EGL device by DRM render node, independently of DISPLAY/Wayland.
        auto query =
            reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
        auto device_string = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
            eglGetProcAddress("eglQueryDeviceStringEXT"));
        auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        require(query && device_string && platform, "EGL device extensions required");
        EGLDeviceEXT devices[16];
        EGLint count{};
        require(query(16, devices, &count), "Cannot enumerate EGL devices");
        for (int i = 0; i < count; i++) {
            auto *path = device_string(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
            if (path && node == path) {
                egl = platform(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
                break;
            }
        }
        require(egl != EGL_NO_DISPLAY, "No EGL device matches render node");
        require(eglInitialize(egl, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_ES_API),
                "Cannot initialize hardware EGL");
        EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
        EGLConfig cfg{};
        EGLint n{};
        require(eglChooseConfig(egl, attrs, &cfg, 1, &n) && n, "No ES3 EGL configuration");
        EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(egl, cfg, EGL_NO_CONTEXT, ca);
        require(context != EGL_NO_CONTEXT &&
                    eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, context),
                "Cannot make surfaceless EGL current");
        auto *name = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
        renderer = name ? name : "";
        require(renderer.find("AMD") != std::string::npos ||
                    renderer.find("Radeon") != std::string::npos,
                "AMD hardware GLES renderer required");
        bgra = va->create_bgra(1920, 1082);
        auto d = va->export_surface(bgra, true);
        auto create =
            reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroy_image =
            reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        auto target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        require(create && destroy_image && target, "EGL DMA-BUF import unavailable");
        EGLint ia[] = {EGL_WIDTH,
                       1920,
                       EGL_HEIGHT,
                       1082,
                       EGL_LINUX_DRM_FOURCC_EXT,
                       EGLint(d.layers[0].drm_format),
                       EGL_DMA_BUF_PLANE0_FD_EXT,
                       d.objects[d.layers[0].object_index[0]].fd,
                       EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                       EGLint(d.layers[0].offset[0]),
                       EGL_DMA_BUF_PLANE0_PITCH_EXT,
                       EGLint(d.layers[0].pitch[0]),
                       EGL_NONE};
        image = create(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, ia);
        for (unsigned i = 0; i < d.num_objects; i++)
            close(d.objects[i].fd);
        require(image != EGL_NO_IMAGE_KHR, "Cannot import writable BGRA surface");
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        target(GL_TEXTURE_2D, image);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                "VA surface is not EGL renderable");
        const char *vs =
            "#version 300 es\nlayout(location=0)in vec2 pos;layout(location=1)in vec2 "
            "uv;layout(location=2)in vec4 col;out vec2 tex;out vec4 tint;void "
            "main(){gl_Position=vec4(pos.x/960.-1.,pos.y/540.-1.,0,1);tex=uv;tint=col;}";
        const char *fs =
            "#version 300 es\nprecision highp float;in vec2 tex;in vec4 tint;uniform sampler2D "
            "glyph;out vec4 outColor;void "
            "main(){outColor=vec4(tint.rgb,tint.a*(tex.x<0.?1.:texture(glyph,tex).r));}";
        auto v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
        program = glCreateProgram();
        glAttachShader(program, v);
        glAttachShader(program, f);
        glLinkProgram(program);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint linked{};
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        require(linked, "Cannot link dashboard shaders");
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        for (unsigned i = 0; i < 3; i++) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, i == 2 ? 4 : 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                  reinterpret_cast<void *>(size_t(i == 0   ? 0
                                                                  : i == 1 ? 8
                                                                           : 16)));
        }
        init_font(font);
        va_check(vaCreateConfig(va->display(), VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                                &config),
                 "Dashboard VPP config");
        va_check(vaCreateContext(va->display(), config, 0, 0, 0, nullptr, 0, &vpp),
                 "Dashboard VPP context");
        GArray *formats = g_array_new(FALSE, FALSE, sizeof(GstVideoFormat));
        GstVideoFormat format = GST_VIDEO_FORMAT_NV12;
        g_array_append_val(formats, format);
        auto *allocator = gst_va_allocator_new(gst_display, formats);
        auto *caps = gst_caps_from_string(
            ("video/x-raw(memory:VAMemory),format=NV12,width=1920,height=1080,framerate=" +
             std::to_string(fps) + "/1,colorimetry=bt709,interlace-mode=progressive")
                .c_str());
        pool = gst_va_pool_new_with_config(caps, 4, 8, VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE,
                                           GST_VA_FEATURE_DISABLED, allocator, nullptr);
        gst_object_unref(allocator);
        gst_caps_unref(caps);
        require(pool && gst_buffer_pool_set_active(pool, TRUE),
                "Cannot activate bounded NV12 encoder pool");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    void init_font(std::string path) {
        // The locked local font keeps headless hosts independent of system fonts.
        if (path.empty())
            if (const char *root = std::getenv("AMDVA_ROOT")) {
                auto local = std::filesystem::path(root) /
                             ".deps/sysroot/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
                if (std::filesystem::exists(local))
                    path = local.string();
            }
        if (path.empty()) {
            require(FcInit(), "Fontconfig unavailable");
            auto *pattern =
                FcNameParse(reinterpret_cast<const FcChar8 *>("sans-serif:weight=demibold"));
            FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
            FcDefaultSubstitute(pattern);
            FcResult result{};
            auto *match = FcFontMatch(nullptr, pattern, &result);
            FcChar8 *file{};
            if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
                path = reinterpret_cast<char *>(file);
            if (match)
                FcPatternDestroy(match);
            FcPatternDestroy(pattern);
        }
        FT_Library ft{};
        require(FT_Init_FreeType(&ft) == 0, "Cannot initialize FreeType");
        FT_Face face{};
        if (FT_New_Face(ft, path.c_str(), 0, &face)) {
            FT_Done_FreeType(ft);
            throw std::runtime_error("Cannot open dashboard font: " + path);
        }
        FT_Set_Pixel_Sizes(face, 0, 96);
        ascender = float(face->size->metrics.ascender) / 64;
        std::vector<unsigned char> pixels(2048 * 1024);
        for (unsigned ch = 32; ch < 127; ch++) {
            if (FT_Load_Char(face, ch, FT_LOAD_RENDER))
                continue;
            const auto &g = face->glyph;
            const unsigned x = (ch % 16) * 128, y = (ch / 16) * 128;
            for (unsigned row = 0; row < g->bitmap.rows; row++)
                for (unsigned col = 0; col < g->bitmap.width; col++)
                    pixels[(y + row) * 2048 + x + col] =
                        g->bitmap.buffer[row * g->bitmap.pitch + col];
            glyphs[ch] = {float(x) / 2048,         float(y) / 1024,       float(g->bitmap.width),
                          float(g->bitmap.rows),   float(g->bitmap_left), float(g->bitmap_top),
                          float(g->advance.x) / 64};
        }
        FT_Done_Face(face);
        FT_Done_FreeType(ft);
        glGenTextures(1, &atlas);
        glBindTexture(GL_TEXTURE_2D, atlas);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 2048, 1024, 0, GL_RED, GL_UNSIGNED_BYTE,
                     pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        require(glGetError() == GL_NO_ERROR, "Font atlas upload failed");
    }
    Vertex vertex(float x, float y, Color c, float u = -1, float v = -1) {
        return {x, y, u, v, c.r, c.g, c.b, c.a};
    }
    void quad(float x, float y, float w, float h, Color c, float u = -1, float v = -1, float du = 0,
              float dv = 0) {
        auto a = vertex(x, y, c, u, v), b = vertex(x + w, y, c, u + du, v),
             d = vertex(x, y + h, c, u, v + dv), e = vertex(x + w, y + h, c, u + du, v + dv);
        vertices.insert(vertices.end(), {a, b, d, b, e, d});
    }
    void triangle(float ax, float ay, float bx, float by, float cx, float cy, Color c) {
        vertices.insert(vertices.end(), {vertex(ax, ay, c), vertex(bx, by, c), vertex(cx, cy, c)});
    }
    void panel(float x, float y, float w, float h) {
        const auto border = color(0x2a3a4e), bg = color(0x111e2c);
        rounded(x, y, w, h, 12, border);
        rounded(x + 1, y + 1, w - 2, h - 2, 11, bg);
    }
    void rounded(float x, float y, float w, float h, float r, Color c) {
        quad(x + r, y, w - 2 * r, h, c);
        quad(x, y + r, r, h - 2 * r, c);
        quad(x + w - r, y + r, r, h - 2 * r, c);
        for (int corner = 0; corner < 4; corner++) {
            const float cx = x + (corner == 0 || corner == 3 ? r : w - r),
                        cy = y + (corner < 2 ? r : h - r);
            const float start = float(corner * 90 + 180) * 3.14159265f / 180;
            for (int i = 0; i < 8; i++) {
                float a = start + i * 3.14159265f / 16, b = a + 3.14159265f / 16;
                triangle(cx, cy, cx + r * std::cos(a), cy + r * std::sin(a), cx + r * std::cos(b),
                         cy + r * std::sin(b), c);
            }
        }
    }
    float width(const std::string &s, float size) {
        float n = 0;
        for (unsigned char ch : s)
            n += glyphs[ch < 127 && ch >= 32 ? ch : '?'].advance * size / 96;
        return n;
    }
    void text(const std::string &s, float x, float y, float size, Color c = white,
              bool right = false) {
        if (right)
            x -= width(s, size);
        float scale = size / 96;
        for (unsigned char ch : s) {
            const auto &g = glyphs[ch < 127 && ch >= 32 ? ch : '?'];
            quad(x + g.left * scale, y + (ascender - g.top) * scale, g.w * scale, g.h * scale, c,
                 g.u, g.v, g.w / 2048, g.h / 1024);
            x += g.advance * scale;
        }
    }
    void fitted(const std::string &s, float x, float y, float size, float max) {
        text(s, x, y, std::min(size, size * max / std::max(1.f, width(s, size))));
    }
    void numeric(const std::string &s, const std::string &unit, float right, float top) {
        const float uw = width(unit, 36);
        text(unit, right, top + 44, 36, muted, true);
        text(s, right - uw - 7, top, 80, white, true);
    }
    void bar(float x, float y, float w, Value n, Value maximum, Color c, bool saturate = false) {
        rounded(x, y, w, 20, 5, faint);
        if (!ratio(n, maximum)) {
            if (saturate && n && maximum && *maximum > 0 && *n > *maximum) {
                quad(x, y, w, 20, c);
                return;
            }
            for (float p = 0; p < w; p += 24)
                quad(x + p, y, std::min(10.f, w - p), 20, color(0x415369));
            return;
        }
        const float used = float(*n / *maximum) * w;
        if (used > 0)
            quad(x, y, used, 20, c);
    }
    void graph(const std::deque<ResourceSample> &h, float x, float y, float w, float ht,
               Value ResourceSample::*field, Color c, double max) {
        if (h.empty())
            return;
        bool have = false;
        float px{}, py{};
        int64_t pt{};
        const auto end = h.back().monotonic_ns;
        for (const auto &s : h) {
            const auto v = s.*field;
            if (!v) {
                have = false;
                continue;
            }
            const float tx = x + w * (1.f - float(end - s.monotonic_ns) / 60e9f),
                        ty = y + ht * (1.f - float(std::clamp(*v / max, 0., 1.)));
            if (tx < x) {
                have = false;
                continue;
            }
            if (have && s.monotonic_ns - pt < 250000000) {
                Color fill = c;
                fill.a = .25f;
                triangle(px, py, tx, ty, px, y + ht, fill);
                triangle(tx, ty, tx, y + ht, px, y + ht, fill);
                const float dx = tx - px, dy = ty - py, len = std::hypot(dx, dy);
                if (len > 0) {
                    const float ox = -dy / len * 1.75f, oy = dx / len * 1.75f;
                    triangle(px + ox, py + oy, tx + ox, ty + oy, px - ox, py - oy, c);
                    triangle(tx + ox, ty + oy, tx - ox, ty - oy, px - ox, py - oy, c);
                }
            }
            px = tx;
            py = ty;
            pt = s.monotonic_ns;
            have = true;
        }
    }
    void compose(const std::deque<ResourceSample> &history, const std::string &cpu,
                 const std::string &gpu) {
        const auto &s = history.back();
        fitted(cpu, 40, 32, 44, 1120);
        fitted(gpu, 40, 90, 44, 1120);
        const time_t seconds = s.wall_ms / 1000;
        tm local{};
        localtime_r(&seconds, &local);
        char time[32]{}, date[32]{};
        strftime(time, sizeof(time), "%H:%M:%S", &local);
        strftime(date, sizeof(date), "%d %b %Y", &local);
        text(time, 1880, 24, 64, white, true);
        text(date, 1880, 100, 36, muted, true);
        Value dram;
        if (s.dram_read_gbs && s.dram_write_gbs)
            dram = *s.dram_read_gbs + *s.dram_write_gbs;
        double peak = 0;
        for (const auto &v : history) {
            if (v.dram_read_gbs)
                peak = std::max(peak, *v.dram_read_gbs);
            if (v.dram_write_gbs)
                peak = std::max(peak, *v.dram_write_gbs);
        }
        const double axis = std::max(20., std::ceil(peak / 20) * 20);
        for (unsigned i = 0; i < 3; i++) {
            const float x = 40 + i * 620;
            panel(x, 172, 600, 466);
            quad(x + 12, 173, 576, 3, i == 0 ? cyan : i == 1 ? purple : amber);
            text(i == 0 ? "CPU" : i == 1 ? "GPU" : "DRAM", x + 26, 214, 36);
            numeric(fmt(i == 0   ? s.cpu_pct
                        : i == 1 ? s.gpu_pct
                                 : dram),
                    i == 2 ? "GB/s" : "%", x + 574, 200);
            if (i == 2) {
                quad(x + 350, 321, 16, 16, amber);
                text("Read", x + 376, 307, 32, muted);
                quad(x + 470, 321, 16, 16, green);
                text("Write", x + 496, 307, 32, muted);
            }
            for (int j = 0; j <= 4; j++)
                quad(x + 26, 352 + j * 65, 548, 1, color(0x29394c));
            if (i < 2)
                graph(history, x + 26, 352, 548, 260,
                      i == 0 ? &ResourceSample::cpu_pct : &ResourceSample::gpu_pct,
                      i == 0 ? cyan : purple, 100);
            else {
                graph(history, x + 26, 352, 548, 260, &ResourceSample::dram_read_gbs, amber, axis);
                graph(history, x + 26, 352, 548, 260, &ResourceSample::dram_write_gbs, green, axis);
            }
        }
        panel(40, 658, 910, 372);
        text("Memory", 67, 711, 36);
        const bool host = ratio(s.host_used_gib, s.host_total_gib),
                   total = host && nonnegative(s.vram_reserved_gib);
        Value capacity, allocated;
        if (total) {
            capacity = *s.host_total_gib + *s.vram_reserved_gib;
            allocated = *s.host_used_gib + *s.vram_reserved_gib;
        }
        numeric(total ? fmt(allocated) + " / " +
                            fmt(capacity, std::floor(*capacity) == *capacity ? 0 : 1)
                      : "N/A",
                "GiB", 923, 686);
        rounded(67, 826, 856, 20, 5, faint);
        if (total) {
            const auto used = 856 * (*s.host_used_gib / *capacity),
                       reserved = 856 * (*s.vram_reserved_gib / *capacity);
            quad(67, 826, used, 20, green);
            quad(67 + used, 826, reserved, 20, purple);
        } else
            bar(67, 826, 856, {}, {}, green);
        for (unsigned i = 0; i < 3; i++) {
            float x = 67 + i * 289;
            quad(x, 867, 16, 16, i == 0 ? green : i == 1 ? purple : faint);
            auto value = i == 0   ? s.host_used_gib
                         : i == 1 ? s.vram_reserved_gib
                         : host   ? Value(*s.host_total_gib - *s.host_used_gib)
                                  : Value{};
            text(std::string(i == 0   ? "Host "
                             : i == 1 ? "Reserved "
                                      : "Available ") +
                     fmt(value) + (value ? " GiB" : ""),
                 x + 26, 855, 26, muted);
        }
        text("GTT", 67, 930, 32);
        text(ratio(s.gtt_gib, s.gtt_total_gib)
                 ? fmt(s.gtt_gib) + " / " + fmt(s.gtt_total_gib, 0) + " GiB"
                 : "N/A",
             481, 930, 32, white, true);
        text("VRAM", 509, 930, 32);
        text(ratio(s.vram_gib, s.vram_reserved_gib)
                 ? fmt(s.vram_gib) + " / " + fmt(s.vram_reserved_gib, 0) + " GiB"
                 : "N/A",
             923, 930, 32, white, true);
        bar(67, 981, 414, s.gtt_gib, s.gtt_total_gib, amber);
        bar(509, 981, 414, s.vram_gib, s.vram_reserved_gib, purple);
        for (unsigned i = 0; i < 3; i++) {
            const float x = 970 + i * (910.f + 16) / 3, w = (910.f - 32) / 3;
            panel(x, 658, w, 372);
            text(i == 0 ? "Power" : i == 1 ? "VCN" : "VPE", x + 18, 686, 36);
            auto value = i == 0 ? s.socket_w : i == 1 ? s.vcn_pct : s.vpe_pct;
            numeric(fmt(value), i == 0 ? "W" : "%", x + w - 18, 832);
            bar(x + 18, 981, w - 36, value, i == 0 ? power_max : 100, i == 0 ? amber : cyan, i == 0);
        }
    }
    GstBuffer *frame(const std::deque<ResourceSample> &history, const std::string &cpu,
                     const std::string &gpu) {
        require(!history.empty(), "No resource samples");
        require(eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, context),
                "EGL context unavailable");
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 2, 1920, 1080);
        glClearColor(.035f, .065f, .105f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        vertices.clear();
        compose(history, cpu, gpu);
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlas);
        glUniform1i(glGetUniformLocation(program, "glyph"), 0);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(),
                     GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, vertices.size());
        auto fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        require(fence, "Cannot fence GPU drawing");
        glFlush();
        auto result = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000);
        glDeleteSync(fence);
        require(result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED,
                "Dashboard GPU draw timeout");
        require(glGetError() == GL_NO_ERROR, "Dashboard GL draw failed");
        GstBuffer *buffer{};
        GstBufferPoolAcquireParams acquire{};
        acquire.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT;
        if (gst_buffer_pool_acquire_buffer(pool, &buffer, &acquire) != GST_FLOW_OK) {
            eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            return nullptr;
        }
        const auto dst = gst_va_buffer_get_surface(buffer);
        if (std::getenv("AMDVA_DASHBOARD_TRACE"))
            std::fprintf(stderr, "VPP bgra=%u dst=%u\n", bgra, dst);
        // Mesa 25.2 EFC skips VPP after 16 equal-region conversions, leaving
        // needs_begin_frame set. A two-row source offset forces explicit VA CSC
        // and prevents the encoder from retaining the mutable RGB draw surface.
        VARectangle source_region{0, 2, 1920, 1080}, output_region{0, 0, 1920, 1080};
        VAProcPipelineParameterBuffer pp{};
        pp.surface = bgra;
        pp.surface_region = &source_region;
        pp.output_region = &output_region;
        pp.surface_color_standard = VAProcColorStandardBT709;
        pp.output_color_standard = VAProcColorStandardBT709;
        pp.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        pp.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
        VABufferID params = VA_INVALID_ID;
        try {
            va_check(vaCreateBuffer(va->display(), vpp, VAProcPipelineParameterBufferType,
                                    sizeof(pp), 1, &pp, &params),
                     "RGB/NV12 conversion parameters");
            va_check(vaBeginPicture(va->display(), vpp, dst), "VPP begin");
            va_check(vaRenderPicture(va->display(), vpp, &params, 1), "VPP render");
            va_check(vaEndPicture(va->display(), vpp), "VPP end");
            va_check(vaSyncSurface(va->display(), dst), "VPP completion");
            vaDestroyBuffer(va->display(), params);
        } catch (...) {
            if (params != VA_INVALID_ID)
                vaDestroyBuffer(va->display(), params);
            gst_buffer_unref(buffer);
            throw;
        }
        eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return buffer;
    }
};
Renderer::Renderer(const std::string &n, const std::string &f, unsigned fps, double w)
    : impl_(std::make_unique<Impl>()) {
    impl_->init(n, f, fps, w);
}
Renderer::~Renderer() = default;
GstVaDisplay *Renderer::display() const { return impl_->gst_display; }
GstBuffer *Renderer::frame(const std::deque<ResourceSample> &h, const std::string &c,
                           const std::string &g) {
    return impl_->frame(h, c, g);
}
std::string Renderer::description() const { return impl_->renderer; }
} // namespace amdva::dashboard
