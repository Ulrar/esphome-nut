#pragma once

#include "ups_driver.h"

namespace esphome {
namespace eaton_nut {

class Eaton5pxDriver : public UpsDriver {
 public:
  const char *manufacturer() const override { return "Eaton"; }
  const char *model() const override { return "5PX 1500i RT2U G2"; }
  const char *firmware() const override { return "01.10.001"; }
  const char *status() const override { return "WAIT"; }
};

}  // namespace eaton_nut
}  // namespace esphome
