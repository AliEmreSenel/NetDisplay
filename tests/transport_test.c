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

static int cancel(void *p) { return *(int *)p; }

int main(void) {
  CHECK(sodium_init() >= 0);
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
    CHECK(nd_frame_send(sockets[0], 5, 0, data, sizeof(data),
                        encrypted ? key : NULL, scratch, cancel, &obsolete, &tx) == 1);
    CHECK(tx.bytes == 0);
    obsolete = 0;
    CHECK(nd_frame_send(sockets[0], 5, 0, data, sizeof(data),
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
  close(sockets[0]); close(sockets[1]);
  free(buffer); free(scratch);
  return 0;
}
