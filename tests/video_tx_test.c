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
static _Atomic unsigned hold_sequence = 1, history[128];
static _Atomic int plaintext;
uint64_t nd_now_ns(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void nap(void) { struct timespec t = {0,1000000}; nanosleep(&t, NULL); }
int nd_frame_send(int fd, uint64_t session, uint32_t seq, const uint8_t *data,
                 size_t size, int keyframe, const uint8_t *key, uint8_t *scratch,
                 int (*obsolete)(void *), void *context, struct nd_tx_stats *stats) {
  (void)fd; (void)session; (void)scratch; (void)keyframe;
  atomic_store(&plaintext, key == NULL);
  assert(data && size == 32); atomic_store(&last_sequence, seq);
  unsigned index = (unsigned)atomic_load(&started);
  assert(index < 128); atomic_store(&history[index], seq);
  atomic_store(&started, (int)index + 1);
  if (seq == atomic_load(&hold_sequence)) while (!atomic_load(&release_first)) nap();
  if (atomic_load(&mode) == 2) { while (!obsolete(context)) nap(); return 1; }
  if (atomic_load(&mode) == 3) { stats->errors++; return -1; }
  if (atomic_load(&mode) == 4 && seq == 200) { stats->errors++; return -1; }
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
static void publish(struct nd_video_tx *tx, unsigned seq, int keyframe) {
  AVPacket *p = packet();
  if (keyframe) p->flags |= AV_PKT_FLAG_KEY;
  nd_video_tx_publish(tx, p, seq);
  assert(!p->data); av_packet_free(&p);
}
static void prediction(void) {
  /* Pending P-frames must neither cancel their I-frame nor replace each other. */
  atomic_store(&started, 0); atomic_store(&release_first, 0);
  atomic_store(&hold_sequence, 100); atomic_store(&mode, 0);
  struct nd_video_tx *tx = nd_video_tx_open(1, 6, NULL, 1, 1); assert(tx);
  publish(tx, 100, 1); await_started(1);
  publish(tx, 101, 0); publish(tx, 102, 0);
  atomic_store(&release_first, 1);
  struct nd_video_tx_stats s = await_results(tx, 3);
  assert(s.complete == 3 && !s.aborted && !s.replaced);
  assert(atomic_load(&history[0]) == 100 && atomic_load(&history[1]) == 101 && atomic_load(&history[2]) == 102);
  nd_video_tx_close(tx);

  /* A failed reference discards its descendants and requests a fresh I. */
  atomic_store(&started, 0); atomic_store(&release_first, 0);
  atomic_store(&hold_sequence, 200); atomic_store(&mode, 4);
  tx = nd_video_tx_open(1, 7, NULL, 1, 1); assert(tx);
  publish(tx, 200, 1); await_started(1);
  publish(tx, 201, 0); publish(tx, 202, 0);
  atomic_store(&release_first, 1);
  s = await_results(tx, 1); assert(s.errors == 1 && s.replaced == 2);
  assert(nd_video_tx_take_keyframe_request(tx));
  assert(!nd_video_tx_take_keyframe_request(tx));
  publish(tx, 203, 0);
  assert(nd_video_tx_snapshot(tx).dependent_dropped == 1);
  publish(tx, 204, 1);
  s = await_results(tx, 2); assert(s.complete == 1);
  assert(atomic_load(&history[1]) == 204);
  nd_video_tx_close(tx);

  /* A stalled network cannot accumulate a growing GOP queue. */
  atomic_store(&started, 0); atomic_store(&release_first, 0);
  atomic_store(&hold_sequence, 300); atomic_store(&mode, 0);
  tx = nd_video_tx_open(1, 8, NULL, 1, 1); assert(tx);
  publish(tx, 300, 1); await_started(1);
  for (unsigned seq = 301; seq <= 304; seq++) publish(tx, seq, 0);
  assert(nd_video_tx_take_keyframe_request(tx));
  publish(tx, 305, 0); publish(tx, 306, 1);
  atomic_store(&release_first, 1);
  s = await_results(tx, 2);
  assert(s.replaced == 3 && s.dependent_dropped == 2 && s.aborted == 1 && s.complete == 1);
  assert(atomic_load(&history[1]) == 306);
  nd_video_tx_close(tx);
}
int main(void) {
  unsigned char key[32] = {0};
  assert(!nd_video_tx_open(-1,1,key,60,0)); assert(!nd_video_tx_open(1,0,key,60,0));
  assert(!nd_video_tx_open(1,1,key,0,0));
  assert(!nd_video_tx_open(1,1,key,UINT16_MAX+1u,0));
  struct nd_video_tx *tx = nd_video_tx_open(1,1,key,60,0); assert(tx);
  AVPacket *p = packet(); nd_video_tx_publish(tx,p,1); assert(!p->data); av_packet_free(&p);
  await_started(1);
  p=packet(); nd_video_tx_publish(tx,p,2); av_packet_free(&p);
  p=packet(); nd_video_tx_publish(tx,p,3); av_packet_free(&p);
  atomic_store(&release_first,1);
  struct nd_video_tx_stats s=await_results(tx,2);
  assert(s.replaced == 1 && s.aborted == 1 && s.complete == 1 && s.bytes == 32);
  assert(atomic_load(&last_sequence)==3); nd_video_tx_close(tx);
  atomic_store(&mode,2); tx=nd_video_tx_open(1,2,key,60,0); assert(tx);
  p=packet(); nd_video_tx_publish(tx,p,4); av_packet_free(&p);
  s=await_results(tx,1); assert(s.aborted==1 && s.complete==0); nd_video_tx_close(tx);
  atomic_store(&mode,3); tx=nd_video_tx_open(1,3,key,60,0); assert(tx);
  p=packet(); nd_video_tx_publish(tx,p,5); av_packet_free(&p);
  s=await_results(tx,1); assert(s.errors==1); nd_video_tx_close(tx);
  atomic_store(&mode,2); tx=nd_video_tx_open(1,4,key,30,0); assert(tx);
  p=packet(); nd_video_tx_publish(tx,p,6); av_packet_free(&p);
  await_started(5); nd_video_tx_close(tx); nd_video_tx_close(NULL);
  atomic_store(&mode,0); tx=nd_video_tx_open(1,5,NULL,60,0); assert(tx);
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
  prediction();
  puts("PASS: TX ownership, dependency order, bounded overflow, reference failure/recovery, deadline cancellation and shutdown");
  return 0;
}
