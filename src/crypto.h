// SPDX-License-Identifier: GPL-2.0-only
#ifndef NETDISPLAY_CRYPTO_H
#define NETDISPLAY_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "control_proto.h"

#define ND_KEY_SIZE 32u
#define ND_TAG_SIZE 16u
#define ND_SALT_SIZE NDC_SALT_SIZE
/* "<32 hex salt>:<64 hex key>" plus the terminator. */
#define ND_PASSWORD_KEY_TEXT (ND_SALT_SIZE * 2u + 1u + ND_KEY_SIZE * 2u + 1u)

/* The salt every password key used before salts were stored per server. */
extern const uint8_t nd_crypto_legacy_salt[ND_SALT_SIZE];

int nd_crypto_load_psk(const char *path, uint8_t key[ND_KEY_SIZE]);
int nd_crypto_password_key(uint8_t key[ND_KEY_SIZE], const char *password,
                           const uint8_t salt[ND_SALT_SIZE]);
/* Accepts "<salt>:<key>" or a bare legacy key, which uses the legacy salt.
 * *legacy is set to 1 for the bare form. */
int nd_crypto_parse_password_key(const char *text, uint8_t salt[ND_SALT_SIZE],
                                 uint8_t key[ND_KEY_SIZE], int *legacy);
void nd_crypto_format_password_key(char out[ND_PASSWORD_KEY_TEXT],
                                   const uint8_t salt[ND_SALT_SIZE],
                                   const uint8_t key[ND_KEY_SIZE]);
/* Reads a file holding a password key in either nd_crypto_parse_password_key
 * form. */
int nd_crypto_load_password_key(const char *path, uint8_t salt[ND_SALT_SIZE],
                                uint8_t key[ND_KEY_SIZE], int *legacy);
int nd_crypto_key_from_hex(uint8_t key[ND_KEY_SIZE], const char *hex);
void nd_crypto_key_to_hex(char hex[ND_KEY_SIZE * 2u + 1u],
                          const uint8_t key[ND_KEY_SIZE]);
void nd_crypto_random(void *buf, size_t len);
int nd_crypto_verify(const uint8_t a[32], const uint8_t b[32]);

/* Keys produced by one authenticated handshake. Each control direction has
 * its own key, and video keys derive from the video key, so nothing a
 * session encrypts depends on the long-term key alone. */
struct nd_session_keys {
  uint8_t client_to_server[ND_KEY_SIZE];
  uint8_t server_to_client[ND_KEY_SIZE];
  uint8_t client_proof[NDC_AUTH_SIZE];
  uint8_t server_proof[NDC_AUTH_SIZE];
  uint8_t video[ND_KEY_SIZE];
};

/* One side of a CPace exchange on ristretto255. The generator is derived
 * from the long-term key and both nonces, so an eavesdropper or an
 * impostor learns nothing that lets it test password guesses offline. */
struct nd_pake {
  uint8_t scalar[32];
  uint8_t share[NDC_SHARE_SIZE];
};

/* Source: after HELLO. Fills challenge->share; the caller has already set
 * flags, nonce and salt. */
int nd_handshake_server_start(struct nd_pake *pake,
                              const uint8_t key[ND_KEY_SIZE],
                              const struct ndc_hello *hello,
                              struct ndc_challenge *challenge);
/* Receiver: after CHALLENGE. Produces AUTH and the session keys. */
int nd_handshake_client(const uint8_t key[ND_KEY_SIZE],
                        const struct ndc_hello *hello,
                        const struct ndc_challenge *challenge,
                        struct ndc_auth *auth, struct nd_session_keys *keys);
/* Source: checks the receiver's AUTH and produces the reply. Fails with
 * EACCES on a wrong key and EPROTO on an invalid share. */
int nd_handshake_server_finish(struct nd_pake *pake,
                               const uint8_t key[ND_KEY_SIZE],
                               const struct ndc_hello *hello,
                               const struct ndc_challenge *challenge,
                               const struct ndc_auth *auth,
                               struct ndc_auth_reply *reply,
                               struct nd_session_keys *keys);
void nd_pake_clear(struct nd_pake *pake);

/* Per-stream UDP key, derived from a session's video key. */
void nd_crypto_stream_key(uint8_t out[ND_KEY_SIZE],
                          const uint8_t video_key[ND_KEY_SIZE],
                          uint64_t stream_id);

/* Writes a complete sealed control message (12-byte header followed by
 * len + NDC_SEAL_OVERHEAD bytes) to out, which needs room for
 * 12 + NDC_MAX_WIRE bytes. seq must never repeat for one key. */
int nd_crypto_seal_control(uint8_t *out, size_t *out_len, uint16_t type,
                           const void *payload, uint32_t len,
                           const uint8_t key[ND_KEY_SIZE], uint64_t seq);
/* Opens the body that followed a sealed header. *len_io is the payload
 * capacity on entry and the payload length on success. */
int nd_crypto_open_control(uint16_t *type, void *payload, uint32_t *len_io,
                           const uint8_t header[12], const uint8_t *body,
                           size_t body_len, const uint8_t key[ND_KEY_SIZE],
                           uint64_t seq);

int nd_crypto_encrypt(uint8_t *out, size_t *out_len, const uint8_t *in,
                      size_t in_len, const uint8_t key[ND_KEY_SIZE],
                      uint64_t session, uint32_t seq);
int nd_crypto_decrypt(uint8_t *out, size_t *out_len, const uint8_t *in,
                      size_t in_len, const uint8_t key[ND_KEY_SIZE],
                      uint64_t session, uint32_t seq);

#endif
