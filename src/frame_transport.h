#ifndef ND_FRAME_TRANSPORT_H
#define ND_FRAME_TRANSPORT_H
#include "proto.h"
#include "crypto.h"
#include <stdatomic.h>
#include <stddef.h>
#include <pthread.h>

uint64_t nd_now_ns(void);
struct nd_tx_stats { uint64_t bytes, eagain, errors; };
/* Cancellation is checked between batches and while waiting for socket space. */
int nd_frame_send(int fd, uint64_t session, uint32_t seq,
                  const uint8_t *data, size_t size, const uint8_t *key,
                  uint8_t *scratch, int (*obsolete)(void *), void *context,
                  struct nd_tx_stats *stats);

struct nd_rx_stats {
  _Atomic unsigned long long complete, partial_drop, latest_overwrite, bad, dup;
  _Atomic unsigned long long complete_bytes, assembly_ns;
};
struct nd_assembly {
  uint64_t session, previous_session, expected_session;
  uint64_t first_ns, complete_ns;
  uint32_t seq, size, count;
  uint16_t fragments;
  int active;
  uint64_t got[(ND_MAX_FRAGS + 63u) / 64u];
};
/* Returns 1 once per completed/decrypted frame, 0 for discarded/incomplete
 * packets. Caller owns the buffer and may swap it after completion. */
int nd_frame_receive(struct nd_assembly *a, struct nd_rx_stats *stats,
                     uint8_t *buffer, const void *packet, size_t length,
                     const uint8_t *key);

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
