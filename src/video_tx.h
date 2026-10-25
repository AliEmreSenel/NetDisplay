// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <libavcodec/packet.h>
struct nd_video_tx;
struct nd_video_tx_stats {
  uint64_t bytes, complete, replaced, aborted, errors, eagain, oversize, dependent_dropped;
  double last_send_ms;
};
/* fd is borrowed until close; NULL key selects plaintext. Both modes share
 * bounded dependency-aware queues and a one-frame send deadline. */
struct nd_video_tx *nd_video_tx_open(int fd, uint64_t session, const uint8_t key[32], unsigned fps, int predicted);
/* Consumes packet ownership, including empty/oversized packets (discarded).
 * Keyframes supersede pending chains; P-frames are sent in decode order. */
void nd_video_tx_publish(struct nd_video_tx *, AVPacket *, uint32_t sequence);
/* Consume a recovery request after a local drop, error or missed deadline. */
int nd_video_tx_take_keyframe_request(struct nd_video_tx *);
struct nd_video_tx_stats nd_video_tx_snapshot(struct nd_video_tx *);
void nd_video_tx_close(struct nd_video_tx *);
