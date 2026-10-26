#!/usr/bin/env bash
# VIDEO=in.h264 MODEL=m.mxr MODEL_INFO=m.json SEGMENT_SECONDS=10 ./annotate-segments.sh out-%05d.mkv
set -euo pipefail
. "$(dirname "$0")/common.sh"
PATTERN="${1:?usage: annotate-segments.sh PATTERN-%05d.mkv}"
SEGMENT_NS=$(( ${SEGMENT_SECONDS:-10} * 1000000000 ))
run "$DECODE ! amddetect model=\"$MODEL\" model-info=\"$MODEL_INFO\" ! amdwatermark ! amdencode codec=$CODEC bitrate=$BITRATE key-int-max=30 ! ${CODEC}parse ! splitmuxsink muxer=matroskamux location=\"$PATTERN\" max-size-time=$SEGMENT_NS"
