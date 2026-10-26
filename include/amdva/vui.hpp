#pragma once
#include <gst/gst.h>
namespace amdva {
// Return a new encoded Annex-B H.264 AU whose SPS signals BT.709 limited range
// (VUI video_signal_type and colour_description); slice NALs are unchanged.
// VA encoders do not emit these VUI fields from input caps alone. Caller owns the
// result; input ownership is unchanged. Throws std::runtime_error on invalid SPS.
GstBuffer *signal_bt709(GstBuffer *input);
} // namespace amdva
