#pragma once
#include "controller.hpp"
#include <QWidget>
#include <functional>
namespace amdva::nvr {
class AiPage final : public QWidget {
  public:
    AiPage(Controller &, const Options &, std::function<void(unsigned, unsigned)> changed,
           QWidget *parent = nullptr);
};
} // namespace amdva::nvr
