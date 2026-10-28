#pragma once
#include "backend/controller.hpp"
namespace amdva::nvr {
std::string validated_uri(std::string);
Options load_config(const std::string &path);
void save_config(const std::string &path, const Options &);
// Config names (h264, h265, av1; cbr, vbr, qvbr) and display names of re-encoding settings.
const char *codec_name(RecordCodec);
const char *codec_label(RecordCodec);
const char *rate_control_name(RateControl);
RecordCodec parse_codec(const std::string &);
RateControl parse_rate_control(const std::string &);
// For example "H.265 VBR 2.0 Mbps 1080p".
std::string transcode_summary(const Transcode &);
} // namespace amdva::nvr
