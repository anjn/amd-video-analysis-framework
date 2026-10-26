#pragma once
// Copy-audit hooks exported by libamdva_copy_audit.so. Run an application with
//   LD_PRELOAD=<prefix>/lib/libamdva_copy_audit.so
// to count CPU mappings, host/device image transfers and VA readbacks. The symbols
// are weak: they are null when the auditor is not preloaded, so production code
// can reference them unconditionally and test code can guard on the address.
extern "C" {
__attribute__((weak)) void amdva_audit_start();
__attribute__((weak)) void amdva_audit_stop();
// Number of forbidden image transfers/mappings seen since start; must stay 0.
__attribute__((weak)) unsigned long long amdva_audit_errors();
// Total audited copies and bytes (bounded metadata transfers included).
__attribute__((weak)) unsigned long long amdva_audit_copies();
__attribute__((weak)) unsigned long long amdva_audit_bytes();
__attribute__((weak)) unsigned long long amdva_audit_layout_queries();
// Also count GL image uploads/readbacks (glReadPixels, glTexSubImage2D, mapped buffers).
__attribute__((weak)) void amdva_audit_gl_images(bool enabled);
// Mark a bounded host<->device transfer of a known pointer/size/kind as metadata
// (see readback_metadata/upload_metadata in amdva/runtime.hpp).
__attribute__((weak)) void amdva_audit_metadata_begin(const void *pointer, unsigned long size,
                                                      int kind);
__attribute__((weak)) void amdva_audit_metadata_end();
}
