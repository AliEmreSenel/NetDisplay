// SPDX-License-Identifier: GPL-2.0-only
#include "frame_receiver.h"
#include <string.h>
#include <time.h>

uint64_t nd_now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int nd_decode_chain_accept(struct nd_decode_chain *chain, uint32_t seq,
                           uint32_t reference, int keyframe) {
  if (!keyframe && (!chain->valid || reference != chain->seq ||
                    seq != reference + 1u)) {
    chain->valid = 0;
    return 0;
  }
  chain->seq = seq;
  chain->valid = 1;
  return 1;
}

struct frame_fragment {
  uint64_t session;
  uint32_t seq, size, magic;
  uint16_t frag, count;
  size_t offset, length, max_plain;
};

static int parse_fragment(struct frame_fragment *f, const void *packet,
                          size_t n, int encrypted) {
  struct nd_hdr h;
  if (n < sizeof(h) || n > ND_UDP_PAYLOAD_MAX) return 0;
  memcpy(&h, packet, sizeof(h));
  f->session = nd_ntoh64(h.session);
  f->seq = ntohl(h.seq);
  f->size = ntohl(h.frame_size);
  f->magic = ntohl(h.magic);
  f->frag = ntohs(h.frag);
  f->count = ntohs(h.frag_count);
  f->max_plain = ND_MAX_FRAME;
  if (f->magic == ND_VIDEO_MAGIC) f->max_plain += sizeof(struct nd_video_meta);
  if ((f->magic != ND_MAGIC && f->magic != ND_VIDEO_MAGIC) || !f->session ||
      !f->size || f->size > f->max_plain + (encrypted ? 16u : 0u) ||
      !f->count || f->count != (f->size + ND_FRAG_DATA - 1) / ND_FRAG_DATA ||
      f->frag >= f->count)
    return 0;
  f->offset = (size_t)f->frag * ND_FRAG_DATA;
  f->length = f->size - f->offset;
  if (f->length > ND_FRAG_DATA) f->length = ND_FRAG_DATA;
  return n == sizeof(h) + f->length;
}

static int assemble_fragment(struct nd_assembly *a, struct nd_rx_stats *st,
                             uint8_t *buffer, const struct frame_fragment *f,
                             const uint8_t *payload, const uint8_t *key) {
  uint64_t session = f->session;
  uint32_t seq = f->seq, size = f->size, magic = f->magic;
  uint16_t frag = f->frag, count = f->count;
  if (a->expected_session && session != a->expected_session) goto obsolete;
  if (session == a->previous_session) goto obsolete;
  if (session != a->session || (int32_t)(seq - a->seq) > 0) {
    if (a->active) atomic_fetch_add(&st->partial_drop, 1);
    if (session != a->session) a->previous_session = a->session;
    a->session = session;
    a->seq = seq;
    a->size = size;
    a->magic = magic;
    a->fragments = count;
    a->count = 0;
    a->active = 1;
    a->first_ns = nd_now_ns();
    memset(a->got, 0, sizeof(a->got));
  } else if (!a->active || seq != a->seq) {
    goto obsolete;
  } else if (size != a->size || count != a->fragments || magic != a->magic) goto bad;
  uint64_t mask = 1ull << (frag & 63u);
  if (a->got[frag >> 6] & mask) {
    atomic_fetch_add(&st->dup, 1);
    return 0;
  }
  memcpy(buffer + f->offset, payload, f->length);
  a->got[frag >> 6] |= mask;
  if (++a->count != a->fragments) return 0;
  a->active = 0;
  a->complete_ns = nd_now_ns();
  if (key) {
    size_t plain;
    if (nd_crypto_decrypt(buffer, &plain, buffer, a->size, key, session, seq) < 0 ||
        !plain || plain > f->max_plain) goto bad;
    a->size = (uint32_t)plain;
  }
  a->offset = 0;
  a->keyframe = 1;
  a->reference = seq;
  if (magic == ND_VIDEO_MAGIC) {
    struct nd_video_meta meta;
    if (a->size <= sizeof(meta)) goto bad;
    memcpy(&meta, buffer, sizeof(meta));
    uint32_t flags = ntohl(meta.flags), reference = ntohl(meta.reference);
    if (ntohl(meta.magic) != ND_VIDEO_MAGIC || (flags & ~ND_VIDEO_KEYFRAME) ||
        reference != ((flags & ND_VIDEO_KEYFRAME) ? seq : seq - 1u)) goto bad;
    a->offset = sizeof(meta);
    a->size -= sizeof(meta);
    a->keyframe = !!(flags & ND_VIDEO_KEYFRAME);
    a->reference = reference;
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
  return -1;
}

int nd_frame_receive(struct nd_assembly *a, struct nd_rx_stats *st,
                     uint8_t *buffer, const void *packet, size_t n,
                     const uint8_t *key) {
  struct frame_fragment f;
  if (!parse_fragment(&f, packet, n, key != NULL)) {
    atomic_fetch_add(&st->bad, 1);
    return 0;
  }
  const uint8_t *payload = (const uint8_t *)packet + sizeof(struct nd_hdr);
  return assemble_fragment(a, st, buffer, &f, payload, key) == 1;
}

void nd_reorder_init(struct nd_reorder *r, uint64_t session, uint8_t *a, uint8_t *b) {
  memset(r, 0, sizeof(*r));
  r->expected_session = session;
  r->slots[0].buffer = a;
  r->slots[1].buffer = b;
}

static void reorder_drop(struct nd_reorder_slot *s, struct nd_rx_stats *st) {
  if (s->used && s->assembly.active) atomic_fetch_add(&st->partial_drop, 1);
  s->used = s->complete = 0;
}

void nd_reorder_feed(struct nd_reorder *r, struct nd_rx_stats *st,
                     const void *packet, size_t n, const uint8_t *key, uint64_t now) {
  struct frame_fragment f;
  /* Validate the envelope before it can evict either slot. */
  if (!parse_fragment(&f, packet, n, key != NULL)) {
    atomic_fetch_add(&st->bad, 1);
    return;
  }
  uint64_t session = f.session;
  uint32_t seq = f.seq, magic = f.magic;
  if ((r->expected_session && session != r->expected_session) ||
      session == r->previous_session) {
    atomic_fetch_add(&st->obsolete, 1);
    return;
  }
  if (!r->slots[0].used) {
    r->deadline_ns = 0;
    if (r->slots[1].used) {
      struct nd_reorder_slot tmp = r->slots[0];
      r->slots[0] = r->slots[1];
      r->slots[1] = tmp;
    }
  }
  struct nd_reorder_slot *old = &r->slots[0], *newer = &r->slots[1], *s = NULL;
  if (r->deadline_ns && now >= r->deadline_ns) {
    reorder_drop(old, st);
    r->deadline_ns = 0;
    r->last_valid = 0;
  }
  if (old->used && old->assembly.session == session && old->assembly.seq == seq) s = old;
  else if (newer->used && newer->assembly.session == session && newer->assembly.seq == seq) s = newer;
  else {
    struct nd_reorder_slot *latest = newer->used ? newer : old->used ? old : NULL;
    if (latest && (latest->assembly.session != session || (int32_t)(seq - latest->assembly.seq) > 0)) {
      if (magic == ND_VIDEO_MAGIC && latest == old && old->assembly.magic == ND_VIDEO_MAGIC &&
          old->assembly.active && seq == old->assembly.seq + 1u && old->assembly.session == session) {
        s = newer;
        r->deadline_ns = now + ND_REORDER_BUDGET_NS;
      } else {
        reorder_drop(old, st); reorder_drop(newer, st);
        r->deadline_ns = 0;
        if (magic == ND_VIDEO_MAGIC) r->last_valid = 0;
        s = old;
      }
    } else if (!latest) {
      if (r->newest_valid && session == r->newest_session &&
          (int32_t)(seq - r->newest_seq) <= 0) { atomic_fetch_add(&st->obsolete, 1); return; }
      s = old;
    } else { atomic_fetch_add(&st->obsolete, 1); return; }
    if (r->newest_valid && session != r->newest_session) {
      r->previous_session = r->newest_session;
      r->last_valid = 0;
    }
    r->newest_seq = seq; r->newest_session = session; r->newest_valid = 1;
    uint8_t *buf = s->buffer;
    memset(s, 0, sizeof(*s));
    s->buffer = buf;
    s->assembly.expected_session = r->expected_session;
    s->used = 1;
  }
  if (s->complete) { atomic_fetch_add(&st->dup, 1); return; }
  const uint8_t *payload = (const uint8_t *)packet + sizeof(struct nd_hdr);
  int result = assemble_fragment(&s->assembly, st, s->buffer, &f, payload, key);
  if (result == 1) {
    s->complete = 1;
    if (s == newer && s->assembly.keyframe) {
      reorder_drop(old, st);
      r->deadline_ns = 0;
    }
  } else if (result < 0 && !s->assembly.active) {
    /* A damaged successor cannot invalidate the predecessor's reference. */
    reorder_drop(s, st);
  }
}

struct nd_reorder_slot *nd_reorder_take(struct nd_reorder *r) {
  struct nd_reorder_slot *old = &r->slots[0], *newer = &r->slots[1];
  struct nd_reorder_slot *s = old->used ? old : newer->used ? newer : NULL;
  if (!s || !s->complete) return NULL;
  if (!s->assembly.keyframe && (!r->last_valid || s->assembly.reference != r->last_seq)) {
    s->used = s->complete = 0;
    r->last_valid = 0;
    return NULL;
  }
  r->last_seq = s->assembly.seq;
  r->last_valid = 1;
  s->used = s->complete = 0;
  if (s == old) r->deadline_ns = 0;
  return s;
}
