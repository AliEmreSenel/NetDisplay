// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "video_tx.h"
#include "frame_transport.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

struct ndvr_tx {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  pthread_t thread;
  int fd, ready, stopped;
  uint64_t session, budget_ns, deadline_ns;
  uint32_t sequence;
  uint8_t key[32], *scratch;
  AVPacket *latest, *sending;
  struct ndvr_tx_stats stats;
};
static int obsolete(void *arg) {
  struct ndvr_tx *t = arg;
  pthread_mutex_lock(&t->mutex);
  int stop = t->stopped || t->ready;
  pthread_mutex_unlock(&t->mutex);
  return stop || nd_now_ns() >= t->deadline_ns;
}
static void *transmit(void *arg) {
  struct ndvr_tx *t = arg;
  for (;;) {
    pthread_mutex_lock(&t->mutex);
    while (!t->stopped && !t->ready) pthread_cond_wait(&t->cond, &t->mutex);
    if (t->stopped) { pthread_mutex_unlock(&t->mutex); break; }
    av_packet_unref(t->sending);
    av_packet_move_ref(t->sending, t->latest);
    uint32_t seq = t->sequence;
    t->ready = 0;
    pthread_mutex_unlock(&t->mutex);
    uint64_t start = nd_now_ns();
    t->deadline_ns = start + t->budget_ns;
    struct nd_tx_stats stats = {0};
    int result = nd_frame_send(t->fd, t->session, seq, t->sending->data,
        (size_t)t->sending->size, t->key, t->scratch, obsolete, t, &stats);
    pthread_mutex_lock(&t->mutex);
    t->stats.bytes += stats.bytes;
    t->stats.complete += result == 0;
    t->stats.aborted += result == 1;
    t->stats.errors += result < 0;
    t->stats.eagain += stats.eagain;
    t->stats.last_send_ms = (nd_now_ns() - start) / 1e6;
    pthread_mutex_unlock(&t->mutex);
    av_packet_unref(t->sending);
  }
  return NULL;
}
struct ndvr_tx *ndvr_tx_open(int fd, uint64_t session, const uint8_t key[32], unsigned fps) {
  if (fd < 0 || !session || fps < 1 || fps > 240 || !key) return NULL;
  struct ndvr_tx *t = calloc(1, sizeof(*t));
  if (!t) return NULL;
  t->fd = fd; t->session = session; t->budget_ns = 1000000000ull / fps;
  memcpy(t->key, key, 32);
  t->scratch = malloc(ND_MAX_WIRE_FRAME);
  t->latest = av_packet_alloc(); t->sending = av_packet_alloc();
  if (!t->scratch || !t->latest || !t->sending) goto fail;
  if (pthread_mutex_init(&t->mutex, NULL)) goto fail;
  if (pthread_cond_init(&t->cond, NULL)) { pthread_mutex_destroy(&t->mutex); goto fail; }
  if (pthread_create(&t->thread, NULL, transmit, t)) {
    pthread_cond_destroy(&t->cond); pthread_mutex_destroy(&t->mutex); goto fail;
  }
  return t;
fail:
  av_packet_free(&t->latest); av_packet_free(&t->sending);
  free(t->scratch); sodium_memzero(t->key, sizeof(t->key)); free(t);
  return NULL;
}
void ndvr_tx_publish(struct ndvr_tx *t, AVPacket *packet, uint32_t sequence) {
  pthread_mutex_lock(&t->mutex);
  t->stats.replaced += t->ready;
  av_packet_unref(t->latest);
  av_packet_move_ref(t->latest, packet);
  t->sequence = sequence; t->ready = 1;
  pthread_cond_signal(&t->cond);
  pthread_mutex_unlock(&t->mutex);
}
struct ndvr_tx_stats ndvr_tx_snapshot(struct ndvr_tx *t) {
  pthread_mutex_lock(&t->mutex);
  struct ndvr_tx_stats value = t->stats;
  pthread_mutex_unlock(&t->mutex);
  return value;
}
void ndvr_tx_close(struct ndvr_tx *t) {
  if (!t) return;
  pthread_mutex_lock(&t->mutex); t->stopped = 1;
  pthread_cond_signal(&t->cond); pthread_mutex_unlock(&t->mutex);
  pthread_join(t->thread, NULL);
  pthread_cond_destroy(&t->cond); pthread_mutex_destroy(&t->mutex);
  av_packet_free(&t->latest); av_packet_free(&t->sending);
  free(t->scratch); sodium_memzero(t->key, sizeof(t->key)); free(t);
}
