// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_TOUCH_INPUT_H
#define ND_TOUCH_INPUT_H
#include <stdint.h>
#include <linux/input.h>
/* Limit this direct device to its advertised capabilities. */
static inline int nd_touch_event_valid(uint16_t type, uint16_t code, int32_t value) {
  if (type == EV_SYN) return code == SYN_REPORT && value == 0;
  if (type == EV_KEY) return code == BTN_TOUCH && (value == 0 || value == 1);
  if (type != EV_ABS) return 0;
  switch (code) {
  case ABS_MT_SLOT: return value >= 0 && value < 10;
  case ABS_MT_TRACKING_ID: return value >= -1 && value <= 65535;
  case ABS_X: case ABS_Y: case ABS_MT_POSITION_X: case ABS_MT_POSITION_Y:
    return value >= 0 && value <= 65535;
  case ABS_MT_PRESSURE: case ABS_MT_TOUCH_MAJOR: return value >= 0 && value <= 255;
  default: return 0;
  }
}
#endif
