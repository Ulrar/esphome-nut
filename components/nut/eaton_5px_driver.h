#pragma once

#include "ups_driver.h"

namespace esphome {
namespace nut {

class Eaton5pxDriver : public UpsDriver {
 public:
  const char *manufacturer() const override { return "Eaton"; }
  const char *model() const override { return "5PX 1500i RT2U G2"; }
  const char *firmware() const override { return "01.10.001"; }
  const char *status() const override {
    if (!this->readings_.online && !this->readings_.on_battery) {
      return "WAIT";
    }
    if (this->readings_.on_battery || this->readings_.discharging) {
      return "OB";
    }
    return "OL";
  }

  UpsSignal classify_field(uint16_t usage_page, uint16_t usage, uint32_t collection_mask) const override {
    if (usage_page == 0x84 && usage == 0x30) {
      if ((collection_mask & COLLECTION_INPUT) != 0) {
        return UpsSignal::INPUT_VOLTAGE;
      }
      if ((collection_mask & COLLECTION_OUTPUT) != 0) {
        return UpsSignal::OUTPUT_VOLTAGE;
      }
    }
    if (usage_page == 0x84 && usage == 0x35) {
      return UpsSignal::LOAD_PERCENT;
    }
    if (usage_page == 0x85 && usage == 0x66) {
      return UpsSignal::BATTERY_CHARGE;
    }
    if (usage_page == 0x85 && usage == 0x68) {
      return UpsSignal::RUNTIME_SECONDS;
    }
    if (usage_page == 0x84 && usage == 0xD0) {
      return UpsSignal::AC_PRESENT;
    }
    if ((usage_page == 0x84 || usage_page == 0x85) && usage == 0x44) {
      return UpsSignal::CHARGING;
    }
    if ((usage_page == 0x84 || usage_page == 0x85) && usage == 0x45) {
      return UpsSignal::DISCHARGING;
    }
    return UpsSignal::NONE;
  }

  void apply_signal_value(UpsSignal signal, float value) override {
    switch (signal) {
      case UpsSignal::INPUT_VOLTAGE:
        this->readings_.input_voltage = value;
        this->readings_.has_input_voltage = true;
        break;
      case UpsSignal::OUTPUT_VOLTAGE:
        this->readings_.output_voltage = value;
        this->readings_.has_output_voltage = true;
        break;
      case UpsSignal::LOAD_PERCENT:
        this->readings_.load_percent = value;
        this->readings_.has_load_percent = true;
        break;
      case UpsSignal::BATTERY_CHARGE:
        this->readings_.battery_charge = value;
        this->readings_.has_battery_charge = true;
        break;
      case UpsSignal::RUNTIME_SECONDS:
        this->readings_.runtime_seconds = value;
        this->readings_.has_runtime_seconds = true;
        break;
      case UpsSignal::AC_PRESENT:
      case UpsSignal::CHARGING:
        this->readings_.charging = value > 0.5f;
        this->readings_.has_charging = true;
        break;
      case UpsSignal::DISCHARGING:
        this->readings_.discharging = value > 0.5f;
        this->readings_.has_discharging = true;
        if (this->readings_.discharging) {
          this->readings_.on_battery = true;
          this->readings_.online = false;
        }
        break;
      case UpsSignal::NONE:
        break;
    }
  }

  void set_ac_present(bool ac_present) override {
    this->readings_.online = ac_present;
    if (!this->readings_.has_discharging || !this->readings_.discharging) {
      this->readings_.on_battery = !ac_present;
    }
  }

  const DriverReadings &readings() const override { return this->readings_; }

 protected:
  DriverReadings readings_{};
};

}  // namespace nut
}  // namespace esphome
