// SPDX-License-Identifier: GPL-2.0-only
#include "common.h"
#include "session.h"

int ndc_recv_expected(int fd, uint16_t expected, void *payload, uint32_t size) {
  uint16_t type = 0;
  uint32_t len = size;
  int r = ndc_recv_msg(fd, &type, payload, &len);
  if (r <= 0 || type != expected || len != size) {
    if (r > 0) errno = type == NDC_REJECT ? EACCES : EPROTO;
    return -1;
  }
  return 0;
}

int nd_session_server_auth(int fd, const struct ndc_hello *hello,
                           const uint8_t *key, int password,
                           enum nd_encryption_policy encryption,
                           struct ndc_challenge *challenge) {
  int encrypted = !!(ntohl(hello->flags) & NDC_FLAG_FRAME_ENCRYPT);
  if ((encrypted && (encryption == ND_ENCRYPTION_OFF || !key)) ||
      (!encrypted && encryption == ND_ENCRYPTION_REQUIRED))
    goto reject;
  *challenge = (struct ndc_challenge){
      .flags = htonl((key ? NDC_FLAG_AUTH_REQUIRED : 0) |
                     (key && password ? NDC_FLAG_PASSWORD : 0) |
                     (encrypted ? NDC_FLAG_FRAME_ENCRYPT : 0)),
  };
  nd_crypto_random(challenge->nonce, sizeof(challenge->nonce));
  if (ndc_send_msg(fd, NDC_CHALLENGE, challenge, sizeof(*challenge)) < 0)
    return -1;
  if (!key) return 0;
  struct ndc_auth auth;
  uint8_t expected[NDC_AUTH_SIZE];
  if (ndc_recv_expected(fd, NDC_AUTH, &auth, sizeof(auth)) < 0) return -1;
  nd_crypto_proof(expected, key, "client", hello->nonce, challenge->nonce);
  if (!nd_crypto_verify(auth.proof, expected)) goto reject;
  nd_crypto_proof(auth.proof, key, "server", hello->nonce, challenge->nonce);
  return ndc_send_msg(fd, NDC_AUTH, &auth, sizeof(auth));
reject:
  (void)ndc_send_msg(fd, NDC_REJECT, NULL, 0);
  errno = EACCES;
  return -1;
}

int nd_session_client_auth(int fd, const uint8_t key[ND_KEY_SIZE],
                           const struct ndc_hello *hello,
                           const struct ndc_challenge *challenge) {
  struct ndc_auth auth, reply;
  nd_crypto_proof(auth.proof, key, "client", hello->nonce, challenge->nonce);
  if (ndc_send_msg(fd, NDC_AUTH, &auth, sizeof(auth)) < 0 ||
      ndc_recv_expected(fd, NDC_AUTH, &reply, sizeof(reply)) < 0)
    return -1;
  nd_crypto_proof(auth.proof, key, "server", hello->nonce, challenge->nonce);
  if (!nd_crypto_verify(reply.proof, auth.proof)) {
    errno = EACCES;
    return -1;
  }
  return 0;
}
