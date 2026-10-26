#include "amdva/egl_import.hpp"
#include <stdexcept>

namespace amdva {
EglDmaBufImporter::EglDmaBufImporter() {
    create_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    destroy_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    target_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!create_ || !destroy_ || !target_)
        throw std::runtime_error("EGL DMA-BUF import unavailable");
}
EGLImageKHR EglDmaBufImporter::import_plane0(EGLDisplay display,
                                             const VADRMPRIMESurfaceDescriptor &d, unsigned width,
                                             unsigned height, uint32_t offset,
                                             uint32_t pitch) const {
    if (d.num_objects < 1 || d.num_layers < 1)
        throw std::runtime_error("Empty VA surface descriptor");
    const EGLint attrs[] = {EGL_WIDTH,
                            EGLint(width),
                            EGL_HEIGHT,
                            EGLint(height),
                            EGL_LINUX_DRM_FOURCC_EXT,
                            EGLint(d.layers[0].drm_format),
                            EGL_DMA_BUF_PLANE0_FD_EXT,
                            d.objects[d.layers[0].object_index[0]].fd,
                            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                            EGLint(offset),
                            EGL_DMA_BUF_PLANE0_PITCH_EXT,
                            EGLint(pitch),
                            EGL_NONE};
    auto image = create_(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (image == EGL_NO_IMAGE_KHR)
        throw std::runtime_error("Cannot import VA surface DMA-BUF into EGL");
    return image;
}
void EglDmaBufImporter::bind_texture(EGLImageKHR image) const {
    target_(GL_TEXTURE_2D, image);
}
void EglDmaBufImporter::destroy(EGLDisplay display, EGLImageKHR image) const noexcept {
    if (image != EGL_NO_IMAGE_KHR)
        destroy_(display, image);
}
} // namespace amdva
