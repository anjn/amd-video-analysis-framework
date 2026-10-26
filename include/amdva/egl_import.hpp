#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstdint>
#include <va/va_drmcommon.h>

namespace amdva {
// Qt-free import of a VA surface export (VaSession::export_surface) as an
// EGLImage/GL texture, without copying pixels. Works with any EGL display on the
// same GPU, surfaceless or windowed. GL calls beyond the texture binding stay with
// the caller so the helper fits raw GLES and Qt's function tables alike.
class EglDmaBufImporter {
  public:
    // Resolves the EGL/GLES extension entry points; throws std::runtime_error when
    // EGL_EXT_image_dma_buf_import or GL_OES_EGL_image is unavailable.
    EglDmaBufImporter();
    // Imports plane 0 of the descriptor (linear BGRA, or the luma plane of NV12
    // sampled as R8). The descriptor's file descriptors stay owned by the caller and
    // may be closed immediately after the call. Throws on failure.
    EGLImageKHR import_plane0(EGLDisplay display, const VADRMPRIMESurfaceDescriptor &descriptor,
                              unsigned width, unsigned height, uint32_t offset,
                              uint32_t pitch) const;
    // Binds the image as the storage of the texture currently bound to GL_TEXTURE_2D.
    void bind_texture(EGLImageKHR image) const;
    void destroy(EGLDisplay display, EGLImageKHR image) const noexcept;

  private:
    PFNEGLCREATEIMAGEKHRPROC create_{};
    PFNEGLDESTROYIMAGEKHRPROC destroy_{};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_{};
};
} // namespace amdva
