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
static _Atomic int plaintext;
uint64_t nd_now_ns(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void nap(void) { struct timespec t = {0,1000000}; nanosleep(&t, NULL); }
int nd_frame_send(int fd, uint64_t session, uint32_t seq, const uint8_t *data,
                 size_t size, const uint8_t *key, uint8_t *scratch,
                 int (*obsolete)(void *), void *context, struct nd_tx_stats *stats) {
  (void)fd; (void)session; (void)scratch;
  atomic_store(&plaintext, key == NULL);
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
static struct nd_video_tx_stats await_results(struct nd_video_tx *tx, unsigned count) {
  struct nd_video_tx_stats s; uint64_t limit = nd_now_ns()+2000000000ull;
  do { s=nd_video_tx_snapshot(tx); if (s.complete+s.aborted+s.errors >= count) return s; nap(); } while(nd_now_ns()<limit);
  assert(!"TX worker timed out"); return s;
}
int main(void) {
  unsigned char key[32] = {0};
  assert(!nd_video_tx_open(-1,1,key,60)); assert(!nd_video_tx_open(1,0,key,60));
  assert(!nd_video_tx_open(1,1,key,0));
  assert(!nd_video_tx_open(1,1,key,UINT16_MAX+1u));
  struct nd_video_tx *tx = nd_video_tx_open(1,1,key,60); assert(tx);
  AVPacket *p = packet(); nd_video_tx_publish(tx,p,1); assert(!p->data); av_packet_free(&p);
  await_started(1);
  p=packet(); nd_video_tx_publish(tx,p,2); av_packet_free(&p);
  p=packet(); nd_video_tx_publish(tx,p,3); av_packet_free(&p);
  atomic_store(&release_first,1);
  struct nd_video_tx_stats s=await_results(tx,2);
  assert(s.replaced == 1 && s.aborted == 1 && s.complete == 1 && s.bytes == 32);
  assert(atomic_load(&last_sequence)==3); nd_video_tx_close(tx);
  atomic_store(&mode,2); tx=nd_video_tx_open(1,2,key,60); assert(tx);
  p=packet(); nd_video_tx_publish(tx,p,4); av_packet_free(&p);
  s=await_results(tx,1); assert(s.aborted==1 && s.complete==0); nd_video_tx_close(tx);
  atomic_store(&mode,3); tx=nd_video_tx_open(1,3,key,60); assert(tx);
  p=packet(); nd_video_tx_publish(tx,p,5); av_packet_free(&p);
  s=await_results(tx,1); assert(s.errors==1); nd_video_tx_close(tx);
  atomic_store(&mode,2); tx=nd_video_tx_open(1,4,key,30); assert(tx);
  p=packet(); nd_video_tx_publish(tx,p,6); av_packet_free(&p);
  await_started(5); nd_video_tx_close(tx); nd_video_tx_close(NULL);
  atomic_store(&mode,0); tx=nd_video_tx_open(1,5,NULL,60); assert(tx);
  unsigned count = atomic_load(&started);
  p=av_packet_alloc(); assert(p); nd_video_tx_publish(tx,p,7);
  assert(!p->data); av_packet_free(&p);
  p=av_packet_alloc(); assert(p); assert(av_new_packet(p,ND_MAX_FRAME+1)==0);
  nd_video_tx_publish(tx,p,8); assert(!p->data); av_packet_free(&p);
  assert(nd_video_tx_snapshot(tx).oversize==2);
  assert(atomic_load(&started)==(int)count);
  p=packet(); nd_video_tx_publish(tx,p,9); av_packet_free(&p);
  s=await_results(tx,1); assert(s.complete==1 && atomic_load(&plaintext));
  nd_video_tx_close(tx);
  puts("PASS: TX ownership, newest-frame replacement, obsolete/deadline cancellation, error accounting and shutdown");
  return 0;
}
