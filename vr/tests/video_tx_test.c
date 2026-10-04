// SPDX-License-Identifier: GPL-2.0-only
// Tests the real TX worker with a deterministic transport test double.
// CI uses real FFmpeg AVPackets; this is not a codec/network performance test.
#define _POSIX_C_SOURCE 200809L
#include "video_tx.h"
#include "frame_transport.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static _Atomic int started, release_first, mode;
static _Atomic unsigned last_sequence;
uint64_t nd_now_ns(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void nap(void) { struct timespec t = {0,1000000}; nanosleep(&t, NULL); }
int nd_frame_send(int fd, uint64_t session, uint32_t seq, const uint8_t *data,
                 size_t size, const uint8_t *key, uint8_t *scratch,
                 int (*obsolete)(void *), void *context, struct nd_tx_stats *stats) {
  (void)fd; (void)session; (void)key; (void)scratch;
  assert(data && size == 32); atomic_store(&last_sequence, seq);
  atomic_fetch_add(&started, 1);
  if (seq == 1) while (!atomic_load(&release_first)) nap();
  if (atomic_load(&mode) == 2) { while (!obsolete(context)) nap(); return 1; }
  if (atomic_load(&mode) == 3) { stats->errors++; return -1; }
  if (obsolete(context)) return 1;
  stats->bytes += size; return 0;
}
static AVPacket *packet(void) {
  AVPacket *p = av_packet_alloc(); assert(p); assert(av_new_packet(p,32) == 0); return p;
}
static void await_started(int count) {
  uint64_t limit = nd_now_ns() + 2000000000ull;
  while (atomic_load(&started) < count && nd_now_ns() < limit) nap();
  assert(atomic_load(&started) >= count);
}
static struct ndvr_tx_stats await_results(struct ndvr_tx *tx, unsigned count) {
  struct ndvr_tx_stats s; uint64_t limit = nd_now_ns()+2000000000ull;
  do { s=ndvr_tx_snapshot(tx); if (s.complete+s.aborted+s.errors >= count) return s; nap(); } while(nd_now_ns()<limit);
  assert(!"TX worker timed out"); return s;
}
int main(void) {
  unsigned char key[32] = {0};
  assert(!ndvr_tx_open(-1,1,key,60)); assert(!ndvr_tx_open(1,0,key,60));
  assert(!ndvr_tx_open(1,1,key,0)); assert(!ndvr_tx_open(1,1,NULL,60));
  struct ndvr_tx *tx = ndvr_tx_open(1,1,key,60); assert(tx);
  AVPacket *p = packet(); ndvr_tx_publish(tx,p,1); assert(!p->data); av_packet_free(&p);
  await_started(1);
  p=packet(); ndvr_tx_publish(tx,p,2); av_packet_free(&p);
  p=packet(); ndvr_tx_publish(tx,p,3); av_packet_free(&p);
  atomic_store(&release_first,1);
  struct ndvr_tx_stats s=await_results(tx,2);
  assert(s.replaced == 1 && s.aborted == 1 && s.complete == 1 && s.bytes == 32);
  assert(atomic_load(&last_sequence)==3); ndvr_tx_close(tx);
  atomic_store(&mode,2); tx=ndvr_tx_open(1,2,key,60); assert(tx);
  p=packet(); ndvr_tx_publish(tx,p,4); av_packet_free(&p);
  s=await_results(tx,1); assert(s.aborted==1 && s.complete==0); ndvr_tx_close(tx);
  atomic_store(&mode,3); tx=ndvr_tx_open(1,3,key,60); assert(tx);
  p=packet(); ndvr_tx_publish(tx,p,5); av_packet_free(&p);
  s=await_results(tx,1); assert(s.errors==1); ndvr_tx_close(tx);
  atomic_store(&mode,2); tx=ndvr_tx_open(1,4,key,30); assert(tx);
  p=packet(); ndvr_tx_publish(tx,p,6); av_packet_free(&p);
  await_started(5); ndvr_tx_close(tx); ndvr_tx_close(NULL);
  puts("PASS: TX ownership, newest-frame replacement, obsolete/deadline cancellation, error accounting and shutdown");
  return 0;
}
