// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "frame_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sodium.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static size_t packet(uint8_t *out, uint64_t session, uint32_t seq,
                      unsigned frag, const uint8_t *data, size_t size) {
  struct nd_hdr h = {htonl(ND_MAGIC), nd_hton64(session), htonl(seq),
      htons((uint16_t)frag), htons((uint16_t)((size + ND_FRAG_DATA - 1) / ND_FRAG_DATA)),
      htonl((uint32_t)size)};
  size_t n = size - frag * ND_FRAG_DATA;
  if (n > ND_FRAG_DATA) n = ND_FRAG_DATA;
  memcpy(out, &h, sizeof(h));
  memcpy(out + sizeof(h), data + frag * ND_FRAG_DATA, n);
  return n + sizeof(h);
}

enum { invalid_video_flag = 2 };

static void reorder_packet(struct nd_reorder *r, struct nd_rx_stats *st,
                           uint32_t seq, uint32_t flags, unsigned frag, uint64_t now) {
  uint8_t data[ND_FRAG_DATA + 20] = {0}, wire[ND_UDP_PAYLOAD_MAX];
  struct nd_video_meta meta = {htonl(ND_VIDEO_MAGIC), htonl(flags),
                               htonl(flags & ND_VIDEO_KEYFRAME ? seq : seq - 1u)};
  memcpy(data, &meta, sizeof(meta));
  size_t n = packet(wire, 8, seq, frag, data, sizeof(data));
  uint32_t magic = htonl(ND_VIDEO_MAGIC); memcpy(wire, &magic, sizeof(magic));
  nd_reorder_feed(r, st, wire, n, NULL, now);
}
static struct nd_reorder_slot *expect_frame(struct nd_reorder *receiver, uint32_t seq) {
  struct nd_reorder_slot *frame = nd_reorder_take(receiver);
  CHECK(frame && frame->assembly.seq == seq);
  return frame;
}

static void reorder_wrap_and_duplicates(struct nd_reorder *receiver, struct nd_rx_stats *stats) {
  reorder_packet(receiver, stats, UINT32_MAX, ND_VIDEO_KEYFRAME, 0, 100);
  reorder_packet(receiver, stats, UINT32_MAX, ND_VIDEO_KEYFRAME, 1, 101);
  expect_frame(receiver, UINT32_MAX);

  reorder_packet(receiver, stats, 0, 0, 0, 200);
  reorder_packet(receiver, stats, 0, 0, 0, 201); /* duplicate predecessor */
  reorder_packet(receiver, stats, 1, 0, 0, 202);
  reorder_packet(receiver, stats, 1, 0, 0, 203); /* duplicate successor */
  reorder_packet(receiver, stats, 1, 0, 1, 204);
  CHECK(!nd_reorder_take(receiver));

  reorder_packet(receiver, stats, 0, 0, 1, 205);
  expect_frame(receiver, 0);
  expect_frame(receiver, 1);
  CHECK(!nd_reorder_take(receiver) && atomic_load(&stats->dup) == 2);
}

static void reorder_expired_chain(struct nd_reorder *receiver, struct nd_rx_stats *stats) {
  reorder_packet(receiver, stats, 1, ND_VIDEO_KEYFRAME, 0, 100);
  reorder_packet(receiver, stats, 1, ND_VIDEO_KEYFRAME, 1, 101);
  expect_frame(receiver, 1);

  const uint64_t overtaken_ns = 301;
  reorder_packet(receiver, stats, 2, 0, 0, overtaken_ns - 1);
  reorder_packet(receiver, stats, 3, 0, 0, overtaken_ns);
  reorder_packet(receiver, stats, 3, 0, 1, overtaken_ns + 1);
  reorder_packet(receiver, stats, 2, 0, 1, overtaken_ns + ND_REORDER_BUDGET_NS);
  CHECK(!nd_reorder_take(receiver));

  reorder_packet(receiver, stats, 5, 0, 0, 5000000);
  reorder_packet(receiver, stats, 5, 0, 1, 5000001);
  CHECK(!nd_reorder_take(receiver)); /* missing reference */
  reorder_packet(receiver, stats, 6, 0, 0, 5000002);
  reorder_packet(receiver, stats, 7, ND_VIDEO_KEYFRAME, 0, 5000003);
  reorder_packet(receiver, stats, 7, ND_VIDEO_KEYFRAME, 1, 5000004);
  CHECK(expect_frame(receiver, 7)->assembly.keyframe);
  CHECK(atomic_load(&stats->partial_drop) >= 2);
}

static void reorder_bad_successor(struct nd_reorder *receiver, struct nd_rx_stats *stats) {
  reorder_packet(receiver, stats, 10, ND_VIDEO_KEYFRAME, 0, 100);
  reorder_packet(receiver, stats, 10, ND_VIDEO_KEYFRAME, 1, 101);
  expect_frame(receiver, 10);

  reorder_packet(receiver, stats, 11, 0, 0, 200);
  reorder_packet(receiver, stats, 12, invalid_video_flag, 0, 201); /* invalid metadata flag */
  reorder_packet(receiver, stats, 12, invalid_video_flag, 1, 202);
  CHECK(atomic_load(&stats->bad) == 1 && !nd_reorder_take(receiver));
  CHECK(atomic_load(&stats->partial_drop) == 0);

  reorder_packet(receiver, stats, 11, 0, 1, 203);
  expect_frame(receiver, 11);
}

static void reorder_clock_starts_at_zero(struct nd_reorder *receiver, struct nd_rx_stats *stats) {
  reorder_packet(receiver, stats, 20, ND_VIDEO_KEYFRAME, 0, 0);
  reorder_packet(receiver, stats, 21, 0, 0, 0);
  reorder_packet(receiver, stats, 21, 0, 1, 0);
  CHECK(!nd_reorder_take(receiver));

  reorder_packet(receiver, stats, 20, ND_VIDEO_KEYFRAME, 1, 1);
  expect_frame(receiver, 20);
  expect_frame(receiver, 21);
}

static void reorder_keyframe_after_bad_predecessor(struct nd_reorder *receiver,
                                                    struct nd_rx_stats *stats) {
  const uint64_t overtaken_ns = 101;
  reorder_packet(receiver, stats, 30, invalid_video_flag, 0, overtaken_ns - 1);
  reorder_packet(receiver, stats, 31, ND_VIDEO_KEYFRAME, 0, overtaken_ns);
  reorder_packet(receiver, stats, 30, invalid_video_flag, 1, overtaken_ns + 1);
  CHECK(!nd_reorder_take(receiver));

  reorder_packet(receiver, stats, 31, ND_VIDEO_KEYFRAME, 1,
                 overtaken_ns + ND_REORDER_BUDGET_NS);
  CHECK(expect_frame(receiver, 31)->assembly.keyframe);
}

static void reorder_rejects_previous_session(struct nd_reorder *receiver,
                                             struct nd_rx_stats *stats) {
  uint8_t body[] = {1}, wire[ND_UDP_PAYLOAD_MAX];
  size_t size = packet(wire, 8, 1, 0, body, sizeof(body));
  nd_reorder_feed(receiver, stats, wire, size, NULL, 100);
  CHECK(expect_frame(receiver, 1)->assembly.session == 8);
  size = packet(wire, 9, 1, 0, body, sizeof(body));
  nd_reorder_feed(receiver, stats, wire, size, NULL, 101);
  CHECK(expect_frame(receiver, 1)->assembly.session == 9);

  size = packet(wire, 8, 2, 0, body, sizeof(body));
  nd_reorder_feed(receiver, stats, wire, size, NULL, 102);
  CHECK(!nd_reorder_take(receiver));
}

static void reordering(void) {
  uint8_t *older = malloc(ND_MAX_WIRE_FRAME);
  uint8_t *newer = malloc(ND_MAX_WIRE_FRAME);
  CHECK(older && newer);
  const struct {
    void (*run)(struct nd_reorder *, struct nd_rx_stats *);
    uint64_t session;
  } cases[] = {
      {reorder_wrap_and_duplicates, 8},
      {reorder_expired_chain, 8},
      {reorder_bad_successor, 8},
      {reorder_clock_starts_at_zero, 8},
      {reorder_keyframe_after_bad_predecessor, 8},
      {reorder_rejects_previous_session, 0},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    struct nd_reorder receiver;
    struct nd_rx_stats stats = {0};
    nd_reorder_init(&receiver, cases[i].session, older, newer);
    cases[i].run(&receiver, &stats);
  }
  free(older);
  free(newer);
}

static int cancel(void *p) { return *(int *)p; }

static void prediction_queue(void) {
  uint8_t storage[ND_VIDEO_QUEUE_CAPACITY][1], rx[1], decode[1];
  uint8_t *buffers[ND_VIDEO_QUEUE_CAPACITY];
  for (unsigned i = 0; i < ND_VIDEO_QUEUE_CAPACITY; i++) buffers[i] = storage[i];
  struct nd_frame_queue queue;
  CHECK(nd_frame_queue_init(&queue, buffers) == 0);
  struct nd_frame_slot producer = {.buffer = rx, .seq = 9}, consumer = {.buffer = decode};
  CHECK(nd_frame_queue_publish(&queue, &producer) == 1); /* no key yet */
  producer.seq = 10; producer.keyframe = 1;
  CHECK(!nd_frame_queue_publish(&queue, &producer));
  producer.seq = 11; producer.keyframe = 0;
  CHECK(!nd_frame_queue_publish(&queue, &producer));
  producer.seq = 12;
  CHECK(!nd_frame_queue_publish(&queue, &producer));
  for (unsigned seq = 10; seq <= 12; seq++) {
    CHECK(nd_frame_queue_take(&queue, &consumer, 0));
    CHECK(consumer.seq == seq); /* never replace a required reference */
  }
  CHECK(!nd_frame_queue_pending(&queue));
  for (unsigned seq = 13; seq <= 15; seq++) {
    producer.seq = seq;
    CHECK(!nd_frame_queue_publish(&queue, &producer));
  }
  producer.seq = 16;
  CHECK(nd_frame_queue_publish(&queue, &producer) == 4); /* bounded overflow */
  CHECK(!nd_frame_queue_take(&queue, &consumer, 0));
  producer.seq = 17;
  CHECK(nd_frame_queue_publish(&queue, &producer) == 1);
  producer.seq = 18; producer.keyframe = 1;
  CHECK(!nd_frame_queue_publish(&queue, &producer));
  producer.seq = 19; producer.keyframe = 0;
  CHECK(!nd_frame_queue_publish(&queue, &producer));
  producer.seq = 21; producer.keyframe = 1;
  CHECK(nd_frame_queue_publish(&queue, &producer) == 2); /* fresh I supersedes GOP */
  CHECK(nd_frame_queue_take(&queue, &consumer, 0) && consumer.seq == 21);
  nd_frame_queue_destroy(&queue);

  struct nd_decode_chain chain = {0};
  CHECK(!nd_decode_chain_accept(&chain, 1, 0, 0));
  CHECK(nd_decode_chain_accept(&chain, 10, 10, 1));
  CHECK(nd_decode_chain_accept(&chain, 11, 10, 0));
  CHECK(!nd_decode_chain_accept(&chain, 13, 12, 0)); /* lost P */
  CHECK(!nd_decode_chain_accept(&chain, 14, 13, 0));
  CHECK(nd_decode_chain_accept(&chain, UINT32_MAX, UINT32_MAX, 1));
  CHECK(nd_decode_chain_accept(&chain, 0, UINT32_MAX, 0));
  chain.valid = 0; /* decoder failure also breaks the chain */
  CHECK(!nd_decode_chain_accept(&chain, 1, 0, 0));
  CHECK(nd_decode_chain_accept(&chain, 3, 3, 1));
}

static void video_metadata(int sockets[2], const uint8_t key[32], uint8_t *buffer, uint8_t *scratch) {
  uint8_t data[ND_FRAG_DATA + 11] = {0};
  for (int encrypted = 0; encrypted <= 1; encrypted++) {
    for (int keyframe = 0; keyframe <= 1; keyframe++) {
      uint8_t packets[2][ND_UDP_PAYLOAD_MAX];
      size_t lengths[2];
      struct nd_tx_stats tx = {0};
      CHECK(nd_frame_send(sockets[0], 8, 4, data, sizeof(data), keyframe,
                           encrypted ? key : NULL, scratch, NULL, NULL, &tx) == 0);
      for (unsigned i = 0; i < 2; i++) {
        ssize_t n = recv(sockets[1], packets[i], sizeof(packets[i]), 0);
        CHECK(n > 0); lengths[i] = (size_t)n;
      }
      struct nd_assembly a = {.expected_session = 8};
      struct nd_rx_stats st = {0};
      for (int i = 1; i >= 0; i--)
        CHECK(nd_frame_receive(&a, &st, buffer, packets[i], lengths[i], encrypted ? key : NULL) == (i == 0));
      CHECK(a.keyframe == keyframe && a.reference == (keyframe ? 4u : 3u));
      CHECK(a.size == sizeof(data) && a.offset == sizeof(struct nd_video_meta));
      CHECK(!memcmp(buffer + a.offset, data, sizeof(data)));
      /* Reference metadata is covered by AEAD, not merely an unauthenticated
       * header flag. Invalid flags are also rejected in plaintext mode. */
      packets[0][sizeof(struct nd_hdr) + 4] ^= 0x80;
      a = (struct nd_assembly){.expected_session = 8};
      for (unsigned i = 0; i < 2; i++)
        CHECK(!nd_frame_receive(&a, &st, buffer, packets[i], lengths[i], encrypted ? key : NULL));
      CHECK(atomic_load(&st.bad) == 1);
    }
  }
}

int main(void) {
  CHECK(sodium_init() >= 0);
  prediction_queue();
  uint8_t data[ND_FRAG_DATA + 11], wire[ND_UDP_PAYLOAD_MAX];
  uint8_t *buffer = malloc(ND_MAX_WIRE_FRAME), *scratch = malloc(ND_MAX_WIRE_FRAME);
  CHECK(buffer && scratch);
  uint8_t slot_buffer[1], producer_buffer[1], consumer_buffer[1];
  struct nd_latest mailbox;
  CHECK(nd_latest_init(&mailbox, slot_buffer) == 0);
  struct nd_frame_slot producer = {.buffer = producer_buffer, .seq = 1};
  struct nd_frame_slot consumer = {.buffer = consumer_buffer};
  CHECK(!nd_latest_take(&mailbox, &consumer, 0));
  CHECK(!nd_latest_publish(&mailbox, &producer));
  CHECK(producer.buffer == slot_buffer);
  producer.seq = 2;
  CHECK(nd_latest_publish(&mailbox, &producer));
  CHECK(nd_latest_take(&mailbox, &consumer, 1));
  CHECK(consumer.seq == 2 && consumer.buffer == slot_buffer);
  CHECK(!nd_latest_take(&mailbox, &consumer, 0));
  CHECK(mailbox.slot.buffer == consumer_buffer && producer.buffer == producer_buffer);
  nd_latest_destroy(&mailbox);
  randombytes_buf(data, sizeof(data));
  struct nd_assembly a = {0};
  struct nd_rx_stats st = {0};
  size_t n = packet(wire, 1, UINT32_MAX, 1, data, sizeof(data));
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  CHECK(atomic_load(&st.dup) == 1);
  n = packet(wire, 1, UINT32_MAX, 0, data, sizeof(data));
  CHECK(nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  CHECK(!memcmp(data, buffer, sizeof(data)));
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  n = packet(wire, 1, 0, 0, data, sizeof(data));
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  n = packet(wire, 1, 1, 1, data, sizeof(data));
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  CHECK(atomic_load(&st.partial_drop) == 1);
  n = packet(wire, 1, 0, 1, data, sizeof(data));
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  n = packet(wire, 1, 1, 0, data, sizeof(data));
  CHECK(nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  CHECK(atomic_load(&st.complete) == 2);
  CHECK(atomic_load(&st.complete_bytes) == 2 * sizeof(data));
  CHECK(a.complete_ns >= a.first_ns);
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n - 1, NULL));
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, ND_UDP_PAYLOAD_MAX + 1, NULL));
  CHECK(atomic_load(&st.bad) == 2);
  n = packet(wire, 2, 0, 0, data, 1);
  CHECK(nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  n = packet(wire, 1, 20, 0, data, 1);
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n, NULL));
  a.expected_session = 2;
  n = packet(wire, 3, 0, 0, data, 1);
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n, NULL));

  int sockets[2];
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
  uint8_t key[ND_KEY_SIZE];
  randombytes_buf(key, sizeof(key));
  for (int encrypted = 0; encrypted <= 1; encrypted++) {
    struct nd_tx_stats tx = {0};
    int obsolete = 1;
    CHECK(nd_frame_send(sockets[0], 5, 0, data, sizeof(data), -1,
                        encrypted ? key : NULL, scratch, cancel, &obsolete, &tx) == 1);
    CHECK(tx.bytes == 0);
    obsolete = 0;
    CHECK(nd_frame_send(sockets[0], 5, 0, data, sizeof(data), -1,
                        encrypted ? key : NULL, scratch, cancel, &obsolete, &tx) == 0);
    a = (struct nd_assembly){.expected_session = 5};
    for (int i = 0; i < 2; i++) {
      ssize_t got = recv(sockets[1], wire, sizeof(wire), 0);
      CHECK(got > 0);
      CHECK(nd_frame_receive(&a, &st, buffer, wire, (size_t)got,
                             encrypted ? key : NULL) == (i == 1));
    }
    CHECK(a.size == sizeof(data) && !memcmp(buffer, data, sizeof(data)));
  }
  size_t cipher_size;
  CHECK(nd_crypto_encrypt(scratch, &cipher_size, data, 20, key, 5, 1) == 0);
  scratch[0] ^= 1;
  n = packet(wire, 5, 1, 0, scratch, cipher_size);
  CHECK(!nd_frame_receive(&a, &st, buffer, wire, n, key));
  CHECK(atomic_load(&st.bad) == 3);
  video_metadata(sockets, key, buffer, scratch);
  reordering();
  close(sockets[0]); close(sockets[1]);
  free(buffer); free(scratch);
  return 0;
}
