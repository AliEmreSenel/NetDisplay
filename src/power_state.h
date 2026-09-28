// SPDX-License-Identifier: GPL-2.0-only
#ifndef NETDISPLAY_POWER_STATE_H
#define NETDISPLAY_POWER_STATE_H

#include "../kernel/netdisplay_power.h"
#include "control_proto.h"
#include <stdint.h>

#define ND_POWER_INTERVAL_MS 2000u
#define ND_POWER_STALE_MS 15000u

struct nd_power_device {
  char name[NDC_NAME_MAX];
  uint32_t count;
  struct nd_power_property properties[ND_POWER_MAX_PROPERTIES];
};
struct nd_power_snapshot {
  uint32_t count;
  struct nd_power_device devices[ND_POWER_MAX_DEVICES];
};
struct nd_power_rx {
  struct nd_power_snapshot *pending;
  uint32_t expected_devices, expected_properties, received_properties;
  uint64_t started_ms;
};

uint64_t nd_power_now_ms(void);
int nd_power_read(const char *root, struct nd_power_snapshot *snapshot);
int nd_power_send(int fd, const struct nd_power_snapshot *snapshot);
/* 1: complete snapshot in *complete (caller frees), 0: pending, -1: invalid. */
int nd_power_receive(struct nd_power_rx *rx, uint16_t type, const void *data,
                     uint32_t length, struct nd_power_snapshot **complete);
void nd_power_rx_clear(struct nd_power_rx *rx);
const char *nd_power_get(const struct nd_power_device *device, const char *key);

#endif
