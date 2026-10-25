// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_FRAME_RECEIVER_H
#define ND_FRAME_RECEIVER_H
#include "proto.h"
#include "crypto.h"
#include <stdatomic.h>
#include <stddef.h>

/* Portable protocol implementation shared by Linux and iOS. */
uint64_t nd_now_ns(void);
struct nd_rx_stats {
  _Atomic unsigned long long complete, partial_drop, latest_overwrite, bad, dup, obsolete;
  _Atomic unsigned long long complete_bytes, assembly_ns;
};
struct nd_assembly {
  uint64_t session, previous_session, expected_session;
  uint64_t first_ns, complete_ns;
  uint32_t seq, size, count, magic, offset, reference;
  uint16_t fragments;
  int active, keyframe;
  uint64_t got[(ND_MAX_FRAGS + 63u) / 64u];
};
/* Update only when submitting a packet to the decoder; invalidate on decode
 * failure. Missing any prerequisite makes the rest of that chain unusable. */
struct nd_decode_chain { uint32_t seq; int valid; };
int nd_decode_chain_accept(struct nd_decode_chain *, uint32_t seq,
                           uint32_t reference, int keyframe);
/* Returns 1 once per completed/decrypted frame, 0 for discarded/incomplete
 * packets. Caller owns the buffer and may swap it after completion. On success,
 * compressed bytes are buffer+offset for size bytes; metadata is already checked. */
int nd_frame_receive(struct nd_assembly *a, struct nd_rx_stats *stats,
                     uint8_t *buffer, const void *packet, size_t length,
                     const uint8_t *key);

#endif
