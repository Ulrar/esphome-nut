#pragma once

namespace esphome {
namespace nut {

class UpsDriver {
 public:
  virtual ~UpsDriver() = default;

  virtual const char *manufacturer() const = 0;
  virtual const char *model() const = 0;
  virtual const char *firmware() const = 0;
  virtual const char *status() const = 0;
};

}  // namespace nut
}  // namespace esphome
