# Shared defaults for the recipe scripts.
: "${VIDEO:?set VIDEO to an H.264 file}"
: "${MODEL:?set MODEL to a prepared .mxr model}"
: "${MODEL_INFO:?set MODEL_INFO to its model-info JSON}"
CODEC="${CODEC:-h264}"
BITRATE="${BITRATE:-6000}"
DECODE="filesrc location=\"$VIDEO\" ! h264parse ! video/x-h264,framerate=30/1 ! vah264dec ! 'video/x-raw(memory:VAMemory),format=NV12'"
run() { echo "gst-launch-1.0 -e $*" >&2; eval "gst-launch-1.0 -e $*"; }
