#!/usr/bin/env bash
# VIDEO=in.h264 MODEL=m.mxr MODEL_INFO=m.json ./annotate-to-file.sh out.mp4
set -euo pipefail
. "$(dirname "$0")/common.sh"
OUT="${1:?usage: annotate-to-file.sh OUTPUT.mp4|OUTPUT.mkv}"
case "$OUT" in *.mkv) MUX=matroskamux ;; *) MUX=mp4mux ;; esac
run "$DECODE ! amddetect model=\"$MODEL\" model-info=\"$MODEL_INFO\" ! amdwatermark ! amdencode codec=$CODEC bitrate=$BITRATE ! ${CODEC}parse ! $MUX ! filesink location=\"$OUT\""
