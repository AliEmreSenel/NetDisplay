// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ndvr_capture;
struct ndvr_capture *ndvr_capture_open(const char *path, unsigned width,
                                       unsigned height);
/* 1 frame, 0 timeout, -1 error. Pixels valid until next call; tightly packed
 * BGRA/RGBA. */
int ndvr_capture_next(struct ndvr_capture *, const uint8_t **pixels, int *bgra,
                      int timeout_ms);
uint64_t ndvr_capture_readback_ns(const struct ndvr_capture *);
void ndvr_capture_close(struct ndvr_capture *);
#ifdef __cplusplus
}
#endif
