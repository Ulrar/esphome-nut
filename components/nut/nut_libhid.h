/* ESPHome port of the path/scaling helpers from NUT drivers/libhid.c
 * (GPL-2.0-or-later, Copyright (C) 2003-2007 Arnaud Quette, Peter
 * Selinger, Charles Lepple, Arjen de Korte, and others).
 *
 * Provides string_to_path/path_to_string against the vendored
 * HID parser, plus logical_to_physical and unit-exponent handling,
 * matching upstream semantics exactly.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nut_hidparser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *usage_name;
  const HIDNode_t usage_code;
} usage_lkp_t;

/* NULL-terminated array of usage tables (global HID table first, then
 * the MGE vendor-specific table). */
typedef usage_lkp_t *usage_tables_t;

int string_to_path(const char *string, HIDPath_t *path, usage_tables_t *utab);
int path_to_string(char *string, size_t size, const HIDPath_t *path, usage_tables_t *utab);

/* Resolve a dotted HID path (e.g. "UPS.PowerConverter.Input.[1].Voltage")
 * against a parsed report descriptor. Returns NULL if not found. */
HIDData_t *nut_hid_find_object(HIDDesc_t *desc, const char *hidpath);

/* Convert a raw logical value to the physical value NUT would report,
 * applying logical->physical scaling and the HID unit exponent. */
double nut_hid_scale_value(const HIDData_t *data, long logical);

/* Vendored usage lookup tables. */
extern usage_lkp_t nut_hid_usage_lkp[];
extern usage_lkp_t nut_mge_usage_lkp[];

#ifdef __cplusplus
}
#endif
