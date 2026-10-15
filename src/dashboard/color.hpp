#pragma once
#include <gst/gst.h>
namespace amdva::dashboard {
// Return a new encoded Annex-B AU with BT.709 limited SPS; slices are unchanged.
// Caller owns the result; input ownership is unchanged. Throws on invalid SPS.
GstBuffer *signal_bt709(GstBuffer *input);
} // namespace amdva::dashboard
