// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_FRAME_TRANSPORT_H
#define ND_FRAME_TRANSPORT_H
#include "frame_receiver.h"
#include <stddef.h>
#include <pthread.h>

struct nd_tx_stats { uint64_t bytes, eagain, errors; };
/* Cancellation is checked between batches and while waiting for socket space. */
int nd_frame_send(int fd, uint64_t session, uint32_t seq,
                  const uint8_t *data, size_t size, const uint8_t *key,
                  uint8_t *scratch, int (*obsolete)(void *), void *context,
                  struct nd_tx_stats *stats);

struct nd_frame_slot {
  uint8_t *buffer;
  uint32_t size, seq;
  uint64_t session, first_ns, complete_ns;
};
struct nd_latest {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  struct nd_frame_slot slot;
  int ready;
};
int nd_latest_init(struct nd_latest *m, uint8_t *buffer);
/* Ownership swaps, never copies or queues frames. Publish returns 1 when it
 * replaced an unconsumed frame; take returns 1 when a frame was available. */
int nd_latest_publish(struct nd_latest *m, struct nd_frame_slot *frame);
int nd_latest_take(struct nd_latest *m, struct nd_frame_slot *frame, int wait);
void nd_latest_destroy(struct nd_latest *m);
#endif
