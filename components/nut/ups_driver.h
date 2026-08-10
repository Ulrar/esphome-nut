#pragma once

#include <cstdint>

namespace esphome {
namespace nut {

enum class UpsSignal : uint8_t {
  NONE = 0,
  INPUT_VOLTAGE,
  INPUT_FREQUENCY,
  INPUT_CURRENT,
  OUTPUT_VOLTAGE,
  OUTPUT_FREQUENCY,
  OUTPUT_CURRENT,
  OUTPUT_APPARENT_POWER,
  OUTPUT_ACTIVE_POWER,
  LOAD_PERCENT,
  BATTERY_CHARGE,
  RUNTIME_SECONDS,
  BATTERY_VOLTAGE,
  AC_PRESENT,
  CHARGING,
  DISCHARGING,
};

enum CollectionMask : uint32_t {
  COLLECTION_INPUT = 1 << 0,
  COLLECTION_OUTPUT = 1 << 1,
  COLLECTION_POWER_SUMMARY = 1 << 2,
  COLLECTION_BATTERY_SYSTEM = 1 << 3,
};

struct DriverReadings {
  bool online{false};
  bool on_battery{false};
  bool has_input_voltage{false};
  bool has_output_voltage{false};
  bool has_input_frequency{false};
  bool has_output_frequency{false};
  bool has_input_current{false};
  bool has_output_current{false};
  bool has_output_apparent_power{false};
  bool has_output_active_power{false};
  bool has_load_percent{false};
  bool has_battery_charge{false};
  bool has_runtime_seconds{false};
  bool has_battery_voltage{false};
  bool has_charging{false};
  bool has_discharging{false};
  float input_voltage{0.0f};
  float output_voltage{0.0f};
  float input_frequency{0.0f};
  float output_frequency{0.0f};
  float input_current{0.0f};
  float output_current{0.0f};
  float output_apparent_power{0.0f};
  float output_active_power{0.0f};
  float load_percent{0.0f};
  float battery_charge{0.0f};
  float runtime_seconds{0.0f};
  float battery_voltage{0.0f};
  bool charging{false};
  bool discharging{false};
};

class UpsDriver {
 public:
  virtual ~UpsDriver() = default;

  virtual const char *manufacturer() const = 0;
  virtual const char *model() const = 0;
  virtual const char *firmware() const = 0;
  virtual const char *status() const = 0;
  virtual UpsSignal classify_field(uint16_t usage_page, uint16_t usage, uint32_t collection_mask) const = 0;
  virtual void apply_signal_value(UpsSignal signal, float value) = 0;
  virtual void set_ac_present(bool ac_present) = 0;
  virtual const DriverReadings &readings() const = 0;
};

}  // namespace nut
}  // namespace esphome
