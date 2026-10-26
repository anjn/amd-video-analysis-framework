#!/usr/bin/env bash
# VIDEO=a.h264 VIDEO2=b.h264 MODEL=m.mxr MODEL_INFO=m.json ./multistream-encode.sh out0.mkv out1.mkv
# BATCH_SIZE must match the compiled model (default 1).
set -euo pipefail
. "$(dirname "$0")/common.sh"
: "${VIDEO2:?set VIDEO2 to the second H.264 file}"
OUT0="${1:?usage: multistream-encode.sh OUT0.mkv OUT1.mkv}"
OUT1="${2:?usage: multistream-encode.sh OUT0.mkv OUT1.mkv}"
SECOND="${DECODE/\"$VIDEO\"/\"$VIDEO2\"}"
run "$DECODE ! mux.sink_0 $SECOND ! mux.sink_1 \
  gvastreammux name=mux output-mode=container ! \
  amddetect model=\"$MODEL\" model-info=\"$MODEL_INFO\" batch-size=${BATCH_SIZE:-1} ! gvastreamdemux name=demux \
  demux.src_0 ! queue ! amdwatermark ! amdencode codec=$CODEC bitrate=$BITRATE ! ${CODEC}parse ! matroskamux ! filesink location=\"$OUT0\" \
  demux.src_1 ! queue ! amdwatermark ! amdencode codec=$CODEC bitrate=$BITRATE ! ${CODEC}parse ! matroskamux ! filesink location=\"$OUT1\""
