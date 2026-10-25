// SPDX-License-Identifier: GPL-2.0-only
#include "frame_receiver.h"
#include <string.h>
#include <time.h>

uint64_t nd_now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int nd_frame_receive(struct nd_assembly *a, struct nd_rx_stats *st,
                     uint8_t *buffer, const void *packet, size_t n,
                     const uint8_t *key) {
  struct nd_hdr h;
  if (n < sizeof(h) || n > ND_UDP_PAYLOAD_MAX) goto bad;
  memcpy(&h, packet, sizeof(h));
  uint64_t session = nd_ntoh64(h.session);
  uint32_t seq = ntohl(h.seq), size = ntohl(h.frame_size);
  uint16_t frag = ntohs(h.frag), count = ntohs(h.frag_count);
  if (ntohl(h.magic) != ND_MAGIC || !session || !size ||
      size > (key ? ND_MAX_WIRE_FRAME : ND_MAX_FRAME) || !count ||
      count != (size + ND_FRAG_DATA - 1) / ND_FRAG_DATA || frag >= count)
    goto bad;
  if (a->expected_session && session != a->expected_session) goto obsolete;
  size_t off = (size_t)frag * ND_FRAG_DATA;
  size_t want = size - off;
  if (want > ND_FRAG_DATA) want = ND_FRAG_DATA;
  if (n != sizeof(h) + want) goto bad;
  if (session == a->previous_session) goto obsolete;
  if (session != a->session || (int32_t)(seq - a->seq) > 0) {
    if (a->active) atomic_fetch_add(&st->partial_drop, 1);
    if (session != a->session) a->previous_session = a->session;
    a->session = session;
    a->seq = seq;
    a->size = size;
    a->fragments = count;
    a->count = 0;
    a->active = 1;
    a->first_ns = nd_now_ns();
    memset(a->got, 0, sizeof(a->got));
  } else if (!a->active || seq != a->seq) {
    goto obsolete;
  } else if (size != a->size || count != a->fragments) goto bad;
  uint64_t mask = 1ull << (frag & 63u);
  if (a->got[frag >> 6] & mask) {
    atomic_fetch_add(&st->dup, 1);
    return 0;
  }
  memcpy(buffer + off, (const uint8_t *)packet + sizeof(h), want);
  a->got[frag >> 6] |= mask;
  if (++a->count != a->fragments) return 0;
  a->active = 0;
  a->complete_ns = nd_now_ns();
  if (key) {
    size_t plain;
    if (nd_crypto_decrypt(buffer, &plain, buffer, a->size, key, session, seq) < 0 ||
        !plain || plain > ND_MAX_FRAME) goto bad;
    a->size = (uint32_t)plain;
  }
  atomic_fetch_add(&st->complete, 1);
  atomic_fetch_add(&st->complete_bytes, a->size);
  atomic_fetch_add(&st->assembly_ns, a->complete_ns - a->first_ns);
  return 1;
obsolete:
  atomic_fetch_add(&st->obsolete, 1);
  return 0;
bad:
  atomic_fetch_add(&st->bad, 1);
  return 0;
}
