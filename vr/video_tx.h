// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <libavcodec/packet.h>
struct ndvr_tx;
struct ndvr_tx_stats {
  uint64_t bytes, complete, replaced, aborted, errors, eagain;
  double last_send_ms;
};
struct ndvr_tx *ndvr_tx_open(int fd, uint64_t session, const uint8_t key[32], unsigned fps);
/* Moves packet ownership into one slot. A new complete IDR replaces an old one. */
void ndvr_tx_publish(struct ndvr_tx *, AVPacket *, uint32_t sequence);
struct ndvr_tx_stats ndvr_tx_snapshot(struct ndvr_tx *);
void ndvr_tx_close(struct ndvr_tx *);
