/* Shim for upstream "usb-common.h": the parser only needs the
 * usb_ctrl_charbuf / usb_ctrl_charbufsize typedefs. The actual USB
 * transport is ESP-IDF's usb host stack, driven from nut.cpp. */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef unsigned char *usb_ctrl_charbuf;
typedef uint16_t usb_ctrl_charbufsize;
typedef uint8_t usb_ctrl_repindex;
typedef uint8_t usb_ctrl_strindex;
