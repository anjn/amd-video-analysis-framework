#!/usr/bin/env bash
# Source this file from any working directory. Dependencies stay on this host,
# under the checkout, with no system library replacement or container runtime.
AMDVA_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export AMDVA_ROOT
export AMDVA_ROCM_ROOT="${AMDVA_ROCM_ROOT:-$AMDVA_ROOT/.deps/rocm72/opt/rocm-7.2.0}"
export ROCM_PATH="$AMDVA_ROCM_ROOT"
export HIP_PATH="$AMDVA_ROCM_ROOT"
export HIP_CLANG_PATH="$AMDVA_ROCM_ROOT/lib/llvm/bin"
export PATH="$AMDVA_ROOT/.deps/prefix/bin:$AMDVA_ROOT/.deps/venv/bin:$AMDVA_ROOT/.deps/sysroot/usr/bin:$AMDVA_ROCM_ROOT/bin:$PATH"
export PKG_CONFIG_PATH="$AMDVA_ROOT/.deps/prefix/lib/pkgconfig:$AMDVA_ROOT/.deps/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$AMDVA_ROOT/build:$AMDVA_ROOT/.deps/prefix/lib:$AMDVA_ROCM_ROOT/lib:$AMDVA_ROCM_ROOT/lib/migraphx/lib:$AMDVA_ROOT/.deps/sysroot/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GST_PLUGIN_PATH="$AMDVA_ROOT/build/plugins:$AMDVA_ROOT/.deps/prefix/lib/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH="$AMDVA_ROOT/.deps/prefix/lib/gstreamer-1.0"
export GST_REGISTRY="$AMDVA_ROOT/.deps/gst-registry.bin"
export BISON_PKGDATADIR="$AMDVA_ROOT/.deps/sysroot/usr/share/bison"
# The host login environment overrides gfx1150 as gfx1151. Use real hardware.
unset HSA_OVERRIDE_GFX_VERSION HSA_ENABLE_SDMA
