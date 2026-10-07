#pragma once
#include "amdva/streaming/controller.hpp"
namespace amdva::streaming {
std::string validated_uri(std::string);
Options load_config(const std::string &path);
void save_config(const std::string &path, const Options &);
} // namespace amdva::streaming
