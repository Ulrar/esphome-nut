/* Minimal config shim for the vendored NUT HID parser on ESP-IDF.
 * The upstream project generates this header with autotools; we only
 * need the handful of feature macros the parser sources reference. */
#pragma once

#define NUT_NETVERSION "esphome-nut"
