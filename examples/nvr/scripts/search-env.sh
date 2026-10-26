#!/usr/bin/env bash
# Source after scripts/env.sh. Keep HIP aliases and PyTorch's runtime together.
# EG2_ROOT is the installed prefix (not the source directory).
if [[ -z "${EG2_ROOT:-}" || -z "${EG2_TORCH_ROOT:-}" ]]; then
    echo 'Set EG2_ROOT to the native install prefix and EG2_TORCH_ROOT to the ROCm torch package directory.' >&2
    return 1
fi
if [[ ! -f "$EG2_ROOT/lib/libeg2.so" || ! -f "$EG2_TORCH_ROOT/lib/libtorch.so" ]]; then
    echo 'The configured native EmbeddingGemma 2 or ROCm LibTorch library is missing.' >&2
    return 1
fi
export LD_LIBRARY_PATH="$EG2_ROOT/lib/eg2-hip:$EG2_ROOT/lib:$EG2_TORCH_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export TORCH_ROCM_AOTRITON_ENABLE_EXPERIMENTAL=1
