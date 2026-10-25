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

/* All payloads use the layouts and network byte order in control_proto.h. */
int ndc_recv_expected(int fd, uint16_t type, void *payload, uint32_t size);
/* Sends CHALLENGE and completes mutual authentication. A NULL key means no
 * authentication. Rejects incompatible encryption policy or a bad proof. */
int nd_session_server_auth(int fd, const struct ndc_hello *hello,
                           const uint8_t *key, int password,
                           enum nd_encryption_policy encryption,
                           struct ndc_challenge *challenge);
/* Called after the client has checked CHALLENGE policy and obtained its key. */
int nd_session_client_auth(int fd, const uint8_t key[ND_KEY_SIZE],
                           const struct ndc_hello *hello,
                           const struct ndc_challenge *challenge);
#endif
