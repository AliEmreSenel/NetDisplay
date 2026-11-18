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
  return 0;
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
  return assemble_fragment(a, st, buffer, &f, payload, key);
}

void nd_reorder_init(struct nd_reorder *r, uint64_t session, uint8_t *a, uint8_t *b) {
  memset(r, 0, sizeof(*r));
  r->expected_session = session;
  r->slots[0].buffer = a;
  r->slots[1].buffer = b;
}

static int slot_pending(const struct nd_reorder_slot *slot) {
  return slot->assembly.active || slot->ready;
}

static void discard_frame(struct nd_reorder_slot *slot, struct nd_rx_stats *stats) {
  if (slot->assembly.active) atomic_fetch_add(&stats->partial_drop, 1);
  slot->assembly.active = 0;
  slot->ready = 0;
}

static int slot_matches(const struct nd_reorder_slot *slot,
                        const struct frame_fragment *fragment) {
  return slot_pending(slot) && slot->assembly.session == fragment->session &&
         slot->assembly.seq == fragment->seq;
}

static int can_keep_predecessor(const struct nd_reorder_slot *older,
                                const struct frame_fragment *fragment) {
  const struct nd_assembly *frame = &older->assembly;
  return frame->active && frame->magic == ND_VIDEO_MAGIC &&
         fragment->magic == ND_VIDEO_MAGIC && fragment->session == frame->session &&
         fragment->seq == frame->seq + 1u;
}

static struct nd_reorder_slot *select_slot(struct nd_reorder *reorder,
                                           struct nd_rx_stats *stats,
                                           struct nd_reorder_slot *older,
                                           struct nd_reorder_slot *newer,
                                           const struct frame_fragment *fragment,
                                           uint64_t now_ns) {
  if (slot_matches(older, fragment))
    return older;
  if (slot_matches(newer, fragment))
    return newer;

  struct nd_reorder_slot *latest = NULL;
  if (slot_pending(newer))
    latest = newer;
  else if (slot_pending(older))
    latest = older;

  /* With empty slots, the last seen sequence still rejects delayed packets. */
  uint64_t latest_session = latest ? latest->assembly.session : reorder->newest_session;
  uint32_t latest_seq = latest ? latest->assembly.seq : reorder->newest_seq;
  if (fragment->session == latest_session && (int32_t)(fragment->seq - latest_seq) <= 0)
    return NULL;

  struct nd_reorder_slot *slot;
  if (latest == older && can_keep_predecessor(older, fragment)) {
    reorder->predecessor_deadline_ns = now_ns + ND_REORDER_BUDGET_NS;
    slot = newer;
  } else {
    discard_frame(older, stats);
    discard_frame(newer, stats);
    reorder->predecessor_deadline_ns = 0;
    if (latest && fragment->magic == ND_VIDEO_MAGIC)
      reorder->released.valid = 0;
    slot = older;
  }

  if (reorder->newest_session && fragment->session != reorder->newest_session) {
    reorder->previous_session = reorder->newest_session;
    reorder->released.valid = 0;
  }
  reorder->newest_session = fragment->session;
  reorder->newest_seq = fragment->seq;
  slot->assembly = (struct nd_assembly){.expected_session = reorder->expected_session};
  return slot;
}

void nd_reorder_feed(struct nd_reorder *reorder, struct nd_rx_stats *stats,
                     const void *packet, size_t length, const uint8_t *key,
                     uint64_t now_ns) {
  struct frame_fragment fragment;
  if (!parse_fragment(&fragment, packet, length, key != NULL)) {
    atomic_fetch_add(&stats->bad, 1);
    return;
  }
  if ((reorder->expected_session && fragment.session != reorder->expected_session) ||
      fragment.session == reorder->previous_session) {
    atomic_fetch_add(&stats->obsolete, 1);
    return;
  }

  struct nd_reorder_slot *older = &reorder->slots[reorder->oldest_slot];
  struct nd_reorder_slot *newer = &reorder->slots[1u - reorder->oldest_slot];
  /* Promote the newer slot by swapping roles, keeping its buffer in place. */
  if (!slot_pending(older)) {
    reorder->predecessor_deadline_ns = 0;
    if (slot_pending(newer)) {
      reorder->oldest_slot = 1u - reorder->oldest_slot;
      struct nd_reorder_slot *empty = older;
      older = newer;
      newer = empty;
    }
  }
  if (reorder->predecessor_deadline_ns && now_ns >= reorder->predecessor_deadline_ns) {
    discard_frame(older, stats);
    reorder->predecessor_deadline_ns = 0;
    reorder->released.valid = 0;
  }

  struct nd_reorder_slot *slot = select_slot(reorder, stats, older, newer, &fragment, now_ns);
  if (!slot) {
    atomic_fetch_add(&stats->obsolete, 1);
    return;
  }
  if (slot->ready) {
    atomic_fetch_add(&stats->dup, 1);
    return;
  }

  const uint8_t *payload = (const uint8_t *)packet + sizeof(struct nd_hdr);
  slot->ready = assemble_fragment(&slot->assembly, stats, slot->buffer, &fragment, payload, key);
  if (slot == newer && slot->ready && slot->assembly.keyframe) {
    discard_frame(older, stats);
    reorder->predecessor_deadline_ns = 0;
  }
}

struct nd_reorder_slot *nd_reorder_take(struct nd_reorder *reorder) {
  struct nd_reorder_slot *older = &reorder->slots[reorder->oldest_slot];
  struct nd_reorder_slot *slot = slot_pending(older)
      ? older : &reorder->slots[1u - reorder->oldest_slot];
  if (!slot->ready) return NULL;

  slot->ready = 0;
  if (slot == older) reorder->predecessor_deadline_ns = 0;
  const struct nd_assembly *frame = &slot->assembly;
  if (!nd_decode_chain_accept(&reorder->released, frame->seq, frame->reference, frame->keyframe))
    return NULL;
  return slot;
}
