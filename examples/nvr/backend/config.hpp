#pragma once
#include "backend/controller.hpp"
namespace amdva::nvr {
std::string validated_uri(std::string);
Options load_config(const std::string &path);
void save_config(const std::string &path, const Options &);
} // namespace amdva::nvr
