/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef NETDISPLAY_POWER_UAPI_H
#define NETDISPLAY_POWER_UAPI_H

#include <linux/types.h>

#define ND_POWER_ABI_VERSION 1u
#define ND_POWER_MAX_DEVICES 16u
#define ND_POWER_MAX_PROPERTIES 128u
#define ND_POWER_KEY_SIZE 64u
#define ND_POWER_VALUE_SIZE 384u
#define ND_POWER_NATIVE_NAME_SIZE 128u
#define ND_POWER_DEVICE_PATH "/dev/netdisplay-power"

struct nd_power_property {
	char key[ND_POWER_KEY_SIZE];
	char value[ND_POWER_VALUE_SIZE];
};

/* One complete, native-endian update per write. One device per open fd.
 * Closing the fd unregisters the device. Unknown values must be omitted.
 * Keys and device identity are immutable; reopen to change them.
 */
struct nd_power_native {
	__u32 version;
	__u32 count;
	char name[ND_POWER_NATIVE_NAME_SIZE];
	struct nd_power_property properties[ND_POWER_MAX_PROPERTIES];
};

#endif
