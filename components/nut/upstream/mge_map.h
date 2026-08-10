/* NUT variable -> HID path mapping extracted from drivers/mge-hid.c
 * (GPL-2.0-or-later). See mge_map.c for the full header notice. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *nut_var;
  const char *hid_path;
  const char *format;  /* printf format from upstream mge_hid2nut[] */
  int convert;         /* 0=plain, 1=Kelvin->Celsius, 2=As->Ah */
} mge_map_entry_t;

extern const mge_map_entry_t MGE_READ_MAP[];
extern const size_t MGE_READ_MAP_COUNT;

/* BOOL status paths, from mge_hid2nut[] "BOOL" entries. Each sets or
 * clears a NUT ups.status token when the HID value is nonzero. */
typedef struct {
  const char *hid_path;
  const char *status_set;    /* token added when value != 0, or NULL */
  const char *status_clear;  /* token added when value == 0, or NULL */
} mge_bool_entry_t;

extern const mge_bool_entry_t MGE_BOOL_MAP[];
extern const size_t MGE_BOOL_MAP_COUNT;

#ifdef __cplusplus
}
#endif
