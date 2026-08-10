#pragma once

#include <string>

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
    this->status_cache_ = (this->readings_.on_battery || this->readings_.discharging) ? "OB" : "OL";
    if (this->readings_.charging && !this->readings_.discharging) {
      this->status_cache_ += " CHRG";
    }
    if (this->readings_.discharging) {
      this->status_cache_ += " DISCHRG";
    }
    if (this->readings_.has_battery_charge && this->readings_.battery_charge <= 20.0f) {
      this->status_cache_ += " LB";
    }
    if (this->readings_.has_load_percent && this->readings_.load_percent >= 100.0f) {
      this->status_cache_ += " OVER";
    }
    return this->status_cache_.c_str();
  }

  UpsSignal classify_field(uint16_t usage_page, uint16_t usage, uint32_t collection_mask) const override {
    if (usage_page == 0x84 && usage == 0x30) {
      if ((collection_mask & COLLECTION_INPUT) != 0) {
        return UpsSignal::INPUT_VOLTAGE;
      }
      if ((collection_mask & COLLECTION_OUTPUT) != 0) {
        return UpsSignal::OUTPUT_VOLTAGE;
      }
      if ((collection_mask & COLLECTION_POWER_SUMMARY) != 0) {
        return UpsSignal::BATTERY_VOLTAGE;
      }
    }
    if (usage_page == 0x84 && usage == 0x31) {
      if ((collection_mask & COLLECTION_INPUT) != 0) {
        return UpsSignal::INPUT_CURRENT;
      }
      if ((collection_mask & COLLECTION_OUTPUT) != 0) {
        return UpsSignal::OUTPUT_CURRENT;
      }
    }
    if (usage_page == 0x84 && usage == 0x32) {
      if ((collection_mask & COLLECTION_INPUT) != 0) {
        return UpsSignal::INPUT_FREQUENCY;
      }
      if ((collection_mask & COLLECTION_OUTPUT) != 0) {
        return UpsSignal::OUTPUT_FREQUENCY;
      }
    }
    if (usage_page == 0x84 && usage == 0x33 && (collection_mask & COLLECTION_OUTPUT) != 0) {
      return UpsSignal::OUTPUT_APPARENT_POWER;
    }
    if (usage_page == 0x84 && usage == 0x34 && (collection_mask & COLLECTION_OUTPUT) != 0) {
      return UpsSignal::OUTPUT_ACTIVE_POWER;
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
      case UpsSignal::INPUT_FREQUENCY:
        this->readings_.input_frequency = value;
        this->readings_.has_input_frequency = true;
        break;
      case UpsSignal::OUTPUT_FREQUENCY:
        this->readings_.output_frequency = value;
        this->readings_.has_output_frequency = true;
        break;
      case UpsSignal::INPUT_CURRENT:
        this->readings_.input_current = value;
        this->readings_.has_input_current = true;
        break;
      case UpsSignal::OUTPUT_CURRENT:
        this->readings_.output_current = value;
        this->readings_.has_output_current = true;
        break;
      case UpsSignal::OUTPUT_APPARENT_POWER:
        this->readings_.output_apparent_power = value;
        this->readings_.has_output_apparent_power = true;
        break;
      case UpsSignal::OUTPUT_ACTIVE_POWER:
        this->readings_.output_active_power = value;
        this->readings_.has_output_active_power = true;
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
      case UpsSignal::BATTERY_VOLTAGE:
        this->readings_.battery_voltage = value;
        this->readings_.has_battery_voltage = true;
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
  mutable std::string status_cache_{};
};

}  // namespace nut
}  // namespace esphome
