#pragma once
#include "controller.hpp"
#include <QWidget>
#include <functional>
namespace amdva::nvr {
class AiPage final : public QWidget {
  public:
    AiPage(Controller &, const Options &,
           std::function<void(double, unsigned, unsigned, unsigned)> changed,
           std::function<bool(const std::string &, const std::string &)> model_changed,
           QWidget *parent = nullptr);
};
} // namespace amdva::nvr
