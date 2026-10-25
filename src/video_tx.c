// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "video_tx.h"
#include "frame_transport.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

struct pending_packet {
  AVPacket *packet;
  uint32_t seq;
  uint64_t published_ns;
};
struct nd_video_tx {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  pthread_t thread;
  int fd, stopped, encrypted, predicted, waiting_keyframe, request_keyframe;
  uint64_t session, budget_ns, deadline_ns, epoch, sending_epoch;
  uint32_t last_seq;
  unsigned head, count;
  uint8_t key[32], *scratch;
  struct pending_packet pending[ND_VIDEO_QUEUE_CAPACITY];
  AVPacket *sending;
  struct nd_video_tx_stats stats;
};
/* Called with the mutex held. Invalidate the entire dependent chain. */
static void abandon(struct nd_video_tx *t) {
  t->stats.replaced += t->count;
  for (unsigned i = 0; i < ND_VIDEO_QUEUE_CAPACITY; i++)
    av_packet_unref(t->pending[i].packet);
  t->count = 0;
  t->epoch++;
  t->waiting_keyframe = 1;
  t->request_keyframe = t->predicted;
}
static int obsolete(void *arg) {
  struct nd_video_tx *t = arg;
  pthread_mutex_lock(&t->mutex);
  int stop = t->stopped || t->sending_epoch != t->epoch;
  pthread_mutex_unlock(&t->mutex);
  return stop || nd_now_ns() >= t->deadline_ns;
}
static void *transmit(void *arg) {
  struct nd_video_tx *t = arg;
  for (;;) {
    pthread_mutex_lock(&t->mutex);
    while (!t->stopped && !t->count) pthread_cond_wait(&t->cond, &t->mutex);
    if (t->stopped) { pthread_mutex_unlock(&t->mutex); break; }
    struct pending_packet *next = &t->pending[t->head];
    av_packet_move_ref(t->sending, next->packet);
    uint32_t seq = next->seq;
    /* Include queue residence in the budget; congestion cannot build a FIFO. */
    t->deadline_ns = next->published_ns + t->budget_ns;
    t->sending_epoch = t->epoch;
    t->head = (t->head + 1u) % ND_VIDEO_QUEUE_CAPACITY;
    t->count--;
    pthread_mutex_unlock(&t->mutex);
    uint64_t start = nd_now_ns();
    struct nd_tx_stats stats = {0};
    int keyframe = t->predicted ? !!(t->sending->flags & AV_PKT_FLAG_KEY) : -1;
    int result = nd_frame_send(t->fd, t->session, seq, t->sending->data,
        (size_t)t->sending->size, keyframe, t->encrypted ? t->key : NULL,
        t->scratch, obsolete, t, &stats);
    pthread_mutex_lock(&t->mutex);
    t->stats.bytes += stats.bytes;
    t->stats.complete += result == 0;
    t->stats.aborted += result == 1;
    t->stats.errors += result < 0;
    t->stats.eagain += stats.eagain;
    t->stats.last_send_ms = (nd_now_ns() - start) / 1e6;
    if (result && t->sending_epoch == t->epoch) abandon(t);
    pthread_mutex_unlock(&t->mutex);
    av_packet_unref(t->sending);
  }
  return NULL;
}
struct nd_video_tx *nd_video_tx_open(int fd, uint64_t session, const uint8_t key[32], unsigned fps, int predicted) {
  if (fd < 0 || !session || fps < 1 || fps > UINT16_MAX) return NULL;
  struct nd_video_tx *t = calloc(1, sizeof(*t));
  if (!t) return NULL;
  t->fd = fd; t->session = session; t->budget_ns = 1000000000ull / fps;
  t->encrypted = key != NULL; t->predicted = !!predicted; t->waiting_keyframe = 1;
  if (key) memcpy(t->key, key, ND_KEY_SIZE);
  if (key || predicted) t->scratch = malloc(ND_MAX_WIRE_FRAME);
  t->sending = av_packet_alloc();
  for (unsigned i = 0; i < ND_VIDEO_QUEUE_CAPACITY; i++) {
    t->pending[i].packet = av_packet_alloc();
    if (!t->pending[i].packet) goto fail;
  }
  if (((key || predicted) && !t->scratch) || !t->sending) goto fail;
  if (pthread_mutex_init(&t->mutex, NULL)) goto fail;
  if (pthread_cond_init(&t->cond, NULL)) { pthread_mutex_destroy(&t->mutex); goto fail; }
  if (pthread_create(&t->thread, NULL, transmit, t)) {
    pthread_cond_destroy(&t->cond); pthread_mutex_destroy(&t->mutex); goto fail;
  }
  return t;
fail:
  for (unsigned i = 0; i < ND_VIDEO_QUEUE_CAPACITY; i++) av_packet_free(&t->pending[i].packet);
  av_packet_free(&t->sending);
  free(t->scratch); sodium_memzero(t->key, sizeof(t->key)); free(t);
  return NULL;
}
void nd_video_tx_publish(struct nd_video_tx *t, AVPacket *packet, uint32_t sequence) {
  pthread_mutex_lock(&t->mutex);
  if (packet->size <= 0 || packet->size > (int)ND_MAX_FRAME || !packet->data) {
    t->stats.oversize++;
    if (t->predicted) abandon(t);
    goto discard;
  }
  int keyframe = !t->predicted || (packet->flags & AV_PKT_FLAG_KEY);
  if (keyframe) {
    abandon(t);
    t->waiting_keyframe = t->request_keyframe = 0;
  } else if (t->waiting_keyframe || sequence != t->last_seq + 1u ||
             t->count == ND_VIDEO_QUEUE_CAPACITY) {
    abandon(t);
    t->stats.dependent_dropped++;
    goto discard;
  }
  struct pending_packet *next = &t->pending[(t->head + t->count) % ND_VIDEO_QUEUE_CAPACITY];
  av_packet_move_ref(next->packet, packet);
  next->seq = sequence;
  next->published_ns = nd_now_ns();
  t->last_seq = sequence;
  t->count++;
  pthread_cond_signal(&t->cond);
  pthread_mutex_unlock(&t->mutex);
  return;
discard:
  pthread_mutex_unlock(&t->mutex);
  av_packet_unref(packet);
}
int nd_video_tx_take_keyframe_request(struct nd_video_tx *t) {
  pthread_mutex_lock(&t->mutex);
  int request = t->request_keyframe;
  t->request_keyframe = 0;
  pthread_mutex_unlock(&t->mutex);
  return request;
}
struct nd_video_tx_stats nd_video_tx_snapshot(struct nd_video_tx *t) {
  pthread_mutex_lock(&t->mutex);
  struct nd_video_tx_stats value = t->stats;
  pthread_mutex_unlock(&t->mutex);
  return value;
}
void nd_video_tx_close(struct nd_video_tx *t) {
  if (!t) return;
  pthread_mutex_lock(&t->mutex); t->stopped = 1;
  pthread_cond_signal(&t->cond); pthread_mutex_unlock(&t->mutex);
  pthread_join(t->thread, NULL);
  pthread_cond_destroy(&t->cond); pthread_mutex_destroy(&t->mutex);
  for (unsigned i = 0; i < ND_VIDEO_QUEUE_CAPACITY; i++) av_packet_free(&t->pending[i].packet);
  av_packet_free(&t->sending);
  free(t->scratch); sodium_memzero(t->key, sizeof(t->key)); free(t);
}
