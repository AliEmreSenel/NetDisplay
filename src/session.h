// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_SESSION_H
#define ND_SESSION_H
#include "control_proto.h"
#include "crypto.h"

enum nd_encryption_policy {
  ND_ENCRYPTION_OFF = 0,
  ND_ENCRYPTION_ALLOWED = 1,
  ND_ENCRYPTION_REQUIRED = 2,
};

/* One control connection. Until an authenticated handshake installs keys it
 * frames messages in plaintext; afterwards every message in both directions
 * is encrypted, authenticated and ordered, and plaintext is refused.
 * Callers serialize sends; only one thread receives. */
struct nd_channel {
  int fd;
  int sealed;
  uint64_t tx_seq, rx_seq;
  uint8_t tx_key[ND_KEY_SIZE], rx_key[ND_KEY_SIZE];
};

void nd_channel_init(struct nd_channel *ch, int fd);
/* Wipes the keys. Does not close the descriptor. */
void nd_channel_clear(struct nd_channel *ch);
int ndc_send_msg(struct nd_channel *ch, uint16_t type, const void *payload,
                 uint32_t len);
/* 1 = message, 0 = orderly EOF, -1 = error. */
int ndc_recv_msg(struct nd_channel *ch, uint16_t *type, void *payload,
                 uint32_t *len_io);

/* All payloads use the layouts and network byte order in control_proto.h. */
int ndc_recv_expected(struct nd_channel *ch, uint16_t type, void *payload,
                      uint32_t size);
/* Sends CHALLENGE and completes the handshake. A NULL key means no
 * authentication and leaves the channel in plaintext. salt is non-NULL for
 * a password key. Rejects an incompatible encryption policy or a wrong key.
 * On success with a key the channel is sealed and video_key holds the
 * session's video key. */
int nd_session_server_auth(struct nd_channel *ch, const struct ndc_hello *hello,
                           const uint8_t *key, const uint8_t *salt,
                           enum nd_encryption_policy encryption,
                           struct ndc_challenge *challenge,
                           uint8_t video_key[ND_KEY_SIZE]);
/* Called after the client has checked CHALLENGE policy and obtained its key.
 * Fails with EACCES when the source does not hold the same key. */
int nd_session_client_auth(struct nd_channel *ch, const uint8_t key[ND_KEY_SIZE],
                           const struct ndc_hello *hello,
                           const struct ndc_challenge *challenge,
                           uint8_t video_key[ND_KEY_SIZE]);
#endif
