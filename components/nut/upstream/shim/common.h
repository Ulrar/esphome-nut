/* Shim for upstream "common.h": only fatalx() is used by the
 * vendored HID parser, and only for internal-consistency errors. */
#pragma once

#include <stdlib.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define upslogx(...) ((void) 0)
#define upsdebugx(...) ((void) 0)
#define LOG_ERR 3

static inline void fatalx(int status, const char *fmt, ...) {
  (void) fmt;
  fprintf(stderr, "nut: fatal error in vendored parser\n");
  abort();
  (void) status;
}

#ifdef __cplusplus
}
#endif
