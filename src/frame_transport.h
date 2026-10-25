// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_FRAME_TRANSPORT_H
#define ND_FRAME_TRANSPORT_H
#include "frame_receiver.h"
#include <stddef.h>
#include <pthread.h>

struct nd_tx_stats { uint64_t bytes, eagain, errors; };
/* keyframe: -1 = legacy ND01, 0 = dependent ND02, 1 = independent ND02.
 * Scratch must hold ND_MAX_WIRE_FRAME for ND02 or encryption.
 * Cancellation is checked between batches and while waiting for socket space. */
int nd_frame_send(int fd, uint64_t session, uint32_t seq,
                  const uint8_t *data, size_t size, int keyframe, const uint8_t *key,
                  uint8_t *scratch, int (*obsolete)(void *), void *context,
                  struct nd_tx_stats *stats);

struct nd_frame_slot {
  uint8_t *buffer;
  uint32_t size, seq, offset, reference;
  int keyframe;
  uint64_t session, first_ns, complete_ns;
};
struct nd_frame_queue {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  struct nd_frame_slot slots[ND_VIDEO_QUEUE_CAPACITY];
  unsigned head, count;
  int waiting_keyframe;
};
/* Buffers remain caller-owned. Publish/take swap ownership without copying.
 * A keyframe supersedes queued work. P-frames preserve decode order; overflow
 * abandons the queued chain and waits for a keyframe. Publish returns drops. */
int nd_frame_queue_init(struct nd_frame_queue *, uint8_t *buffers[ND_VIDEO_QUEUE_CAPACITY]);
unsigned nd_frame_queue_publish(struct nd_frame_queue *, struct nd_frame_slot *);
int nd_frame_queue_take(struct nd_frame_queue *, struct nd_frame_slot *, int wait);
int nd_frame_queue_pending(struct nd_frame_queue *);
void nd_frame_queue_destroy(struct nd_frame_queue *);

/* Independent probe messages retain latest-only replacement. */
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
