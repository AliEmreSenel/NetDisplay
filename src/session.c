// SPDX-License-Identifier: GPL-2.0-only
#include "common.h"
#include "session.h"

#include <sodium.h>

void nd_channel_init(struct nd_channel *ch, int fd) {
  memset(ch, 0, sizeof(*ch));
  ch->fd = fd;
}

void nd_channel_clear(struct nd_channel *ch) {
  sodium_memzero(ch->tx_key, sizeof(ch->tx_key));
  sodium_memzero(ch->rx_key, sizeof(ch->rx_key));
  ch->sealed = 0;
}

static void channel_seal(struct nd_channel *ch, const struct nd_session_keys *k,
                         int server) {
  memcpy(ch->tx_key, server ? k->server_to_client : k->client_to_server,
         ND_KEY_SIZE);
  memcpy(ch->rx_key, server ? k->client_to_server : k->server_to_client,
         ND_KEY_SIZE);
  ch->tx_seq = ch->rx_seq = 0;
  ch->sealed = 1;
}

int ndc_send_msg(struct nd_channel *ch, uint16_t type, const void *payload,
                 uint32_t len) {
  if (!ch->sealed)
    return ndc_send_plain(ch->fd, type, payload, len);
  uint8_t wire[sizeof(struct ndc_hdr) + NDC_MAX_WIRE];
  size_t n = 0;
  if (nd_crypto_seal_control(wire, &n, type, payload, len, ch->tx_key,
                             ch->tx_seq) < 0)
    return -1;
  /* A partly written message leaves the stream unusable either way. */
  ch->tx_seq++;
  int r = ndc_write_full(ch->fd, wire, n);
  sodium_memzero(wire, n);
  return r;
}

int ndc_recv_msg(struct nd_channel *ch, uint16_t *type, void *payload,
                 uint32_t *len_io) {
  if (!ch->sealed)
    return ndc_recv_plain(ch->fd, type, payload, len_io);
  uint8_t header[sizeof(struct ndc_hdr)], body[NDC_MAX_WIRE];
  int r = ndc_read_full(ch->fd, header, sizeof(header));
  if (r <= 0)
    return r;
  struct ndc_hdr h;
  memcpy(&h, header, sizeof(h));
  uint32_t len = ntohl(h.length);
  if (ntohl(h.magic) != NDC_MAGIC || ntohs(h.version) != NDC_VERSION ||
      ntohs(h.type) != NDC_SEALED || len < NDC_SEAL_OVERHEAD ||
      len > NDC_MAX_WIRE) {
    errno = EPROTO;
    return -1;
  }
  r = ndc_read_full(ch->fd, body, len);
  if (r <= 0)
    return r < 0 ? -1 : 0;
  if (nd_crypto_open_control(type, payload, len_io, header, body, len,
                             ch->rx_key, ch->rx_seq) < 0)
    return -1;
  ch->rx_seq++;
  return 1;
}

int ndc_recv_expected(struct nd_channel *ch, uint16_t expected, void *payload,
                      uint32_t size) {
  uint16_t type = 0;
  uint32_t len = size;
  int r = ndc_recv_msg(ch, &type, payload, &len);
  if (r <= 0 || type != expected || len != size) {
    if (r > 0)
      errno = type == NDC_REJECT ? EACCES : EPROTO;
    else if (r == 0)
      errno = ECONNRESET;
    return -1;
  }
  return 0;
}

int nd_session_server_auth(struct nd_channel *ch, const struct ndc_hello *hello,
                           const uint8_t *key, const uint8_t *salt,
                           enum nd_encryption_policy encryption,
                           struct ndc_challenge *challenge,
                           uint8_t video_key[ND_KEY_SIZE]) {
  int encrypted = !!(ntohl(hello->flags) & NDC_FLAG_FRAME_ENCRYPT);
  struct nd_pake pake;
  struct nd_session_keys keys;
  struct ndc_auth auth;
  struct ndc_auth_reply reply;
  int result = -1;
  memset(&pake, 0, sizeof(pake));
  memset(&keys, 0, sizeof(keys));
  if ((encrypted && (encryption == ND_ENCRYPTION_OFF || !key)) ||
      (!encrypted && encryption == ND_ENCRYPTION_REQUIRED))
    goto reject;
  *challenge = (struct ndc_challenge){
      .flags = htonl((key ? NDC_FLAG_AUTH_REQUIRED : 0) |
                     (key && salt ? NDC_FLAG_PASSWORD : 0) |
                     (encrypted ? NDC_FLAG_FRAME_ENCRYPT : 0)),
  };
  nd_crypto_random(challenge->nonce, sizeof(challenge->nonce));
  if (key && salt)
    memcpy(challenge->salt, salt, sizeof(challenge->salt));
  if (key && nd_handshake_server_start(&pake, key, hello, challenge) < 0)
    goto done;
  if (ndc_send_msg(ch, NDC_CHALLENGE, challenge, sizeof(*challenge)) < 0)
    goto done;
  if (!key) {
    result = 0;
    goto done;
  }
  if (ndc_recv_expected(ch, NDC_AUTH, &auth, sizeof(auth)) < 0)
    goto done;
  if (nd_handshake_server_finish(&pake, key, hello, challenge, &auth, &reply,
                                 &keys) < 0)
    goto reject;
  if (ndc_send_msg(ch, NDC_AUTH, &reply, sizeof(reply)) < 0)
    goto done;
  channel_seal(ch, &keys, 1);
  memcpy(video_key, keys.video, ND_KEY_SIZE);
  result = 0;
  goto done;
reject:
  (void)ndc_send_msg(ch, NDC_REJECT, NULL, 0);
  errno = EACCES;
done:
  nd_pake_clear(&pake);
  sodium_memzero(&keys, sizeof(keys));
  return result;
}

int nd_session_client_auth(struct nd_channel *ch, const uint8_t key[ND_KEY_SIZE],
                           const struct ndc_hello *hello,
                           const struct ndc_challenge *challenge,
                           uint8_t video_key[ND_KEY_SIZE]) {
  struct ndc_auth auth;
  struct ndc_auth_reply reply;
  struct nd_session_keys keys;
  int result = -1;
  if (nd_handshake_client(key, hello, challenge, &auth, &keys) < 0)
    return -1;
  if (ndc_send_msg(ch, NDC_AUTH, &auth, sizeof(auth)) < 0 ||
      ndc_recv_expected(ch, NDC_AUTH, &reply, sizeof(reply)) < 0)
    goto done;
  if (!nd_crypto_verify(reply.proof, keys.server_proof)) {
    errno = EACCES;
    goto done;
  }
  channel_seal(ch, &keys, 0);
  memcpy(video_key, keys.video, ND_KEY_SIZE);
  result = 0;
done:
  sodium_memzero(&keys, sizeof(keys));
  return result;
}
