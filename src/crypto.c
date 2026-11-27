// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "crypto.h"
#include "proto.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

static int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  c = (unsigned char)tolower(c);
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

static int hex_decode(uint8_t *out, size_t size, const char *hex) {
  if (!hex || strlen(hex) != size * 2u) {
    errno = EINVAL;
    return -1;
  }
  for (size_t i = 0; i < size; i++) {
    int hi = hex_value((unsigned char)hex[2u * i]);
    int lo = hex_value((unsigned char)hex[2u * i + 1u]);
    if (hi < 0 || lo < 0) {
      sodium_memzero(out, size);
      errno = EINVAL;
      return -1;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

int nd_crypto_key_from_hex(uint8_t key[ND_KEY_SIZE], const char *hex) {
  return hex_decode(key, ND_KEY_SIZE, hex);
}

void nd_crypto_key_to_hex(char hex[ND_KEY_SIZE * 2u + 1u],
                          const uint8_t key[ND_KEY_SIZE]) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < ND_KEY_SIZE; i++) {
    hex[2u * i] = digits[key[i] >> 4];
    hex[2u * i + 1u] = digits[key[i] & 15u];
  }
  hex[ND_KEY_SIZE * 2u] = 0;
}

int nd_crypto_load_psk(const char *path, uint8_t key[ND_KEY_SIZE]) {
  if (!path || !*path) {
    errno = ENOENT;
    return -1;
  }
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  uint8_t b[96];
  size_t n = fread(b, 1, sizeof(b), f);
  int read_error = ferror(f);
  int too_long = !feof(f);
  fclose(f);
  if (read_error || too_long) {
    sodium_memzero(b, sizeof(b));
    errno = EIO;
    return -1;
  }
  if (n == ND_KEY_SIZE) {
    memcpy(key, b, ND_KEY_SIZE);
    sodium_memzero(b, sizeof(b));
    return 0;
  }
  while (n && isspace(b[n - 1]))
    n--;
  if (n == ND_KEY_SIZE * 2u) {
    char hex[ND_KEY_SIZE * 2u + 1u];
    memcpy(hex, b, n);
    hex[n] = 0;
    int r = nd_crypto_key_from_hex(key, hex);
    sodium_memzero(hex, sizeof(hex));
    sodium_memzero(b, sizeof(b));
    return r;
  }
  sodium_memzero(b, sizeof(b));
  errno = EINVAL;
  return -1;
}

const uint8_t nd_crypto_legacy_salt[ND_SALT_SIZE] = {
    'N', 'e', 't', 'D', 'i', 's', 'p', 'l',
    'a', 'y', 'P', 'a', 's', 's', 'v', '1'};

int nd_crypto_password_key(uint8_t key[ND_KEY_SIZE], const char *password,
                           const uint8_t salt[ND_SALT_SIZE]) {
  _Static_assert(ND_SALT_SIZE == crypto_pwhash_SALTBYTES,
                 "password salt must match Argon2id");
  if (!password || !*password || !salt) {
    errno = EINVAL;
    return -1;
  }
  if (crypto_pwhash(key, ND_KEY_SIZE, password,
                    (unsigned long long)strlen(password), salt,
                    crypto_pwhash_OPSLIMIT_INTERACTIVE,
                    crypto_pwhash_MEMLIMIT_INTERACTIVE,
                    crypto_pwhash_ALG_ARGON2ID13) != 0) {
    errno = ENOMEM;
    return -1;
  }
  return 0;
}

int nd_crypto_parse_password_key(const char *text, uint8_t salt[ND_SALT_SIZE],
                                 uint8_t key[ND_KEY_SIZE], int *legacy) {
  if (!text) {
    errno = EINVAL;
    return -1;
  }
  const char *colon = strchr(text, ':');
  if (!colon) {
    if (hex_decode(key, ND_KEY_SIZE, text) < 0)
      return -1;
    memcpy(salt, nd_crypto_legacy_salt, ND_SALT_SIZE);
    if (legacy)
      *legacy = 1;
    return 0;
  }
  char salt_hex[ND_SALT_SIZE * 2u + 1u];
  if ((size_t)(colon - text) != sizeof(salt_hex) - 1u) {
    errno = EINVAL;
    return -1;
  }
  memcpy(salt_hex, text, sizeof(salt_hex) - 1u);
  salt_hex[sizeof(salt_hex) - 1u] = 0;
  if (hex_decode(salt, ND_SALT_SIZE, salt_hex) < 0 ||
      hex_decode(key, ND_KEY_SIZE, colon + 1) < 0) {
    sodium_memzero(salt, ND_SALT_SIZE);
    return -1;
  }
  if (legacy)
    *legacy = 0;
  return 0;
}

void nd_crypto_format_password_key(char out[ND_PASSWORD_KEY_TEXT],
                                   const uint8_t salt[ND_SALT_SIZE],
                                   const uint8_t key[ND_KEY_SIZE]) {
  static const char digits[] = "0123456789abcdef";
  size_t n = 0;
  for (size_t i = 0; i < ND_SALT_SIZE; i++) {
    out[n++] = digits[salt[i] >> 4];
    out[n++] = digits[salt[i] & 15u];
  }
  out[n++] = ':';
  nd_crypto_key_to_hex(out + n, key);
}

int nd_crypto_load_password_key(const char *path, uint8_t salt[ND_SALT_SIZE],
                                uint8_t key[ND_KEY_SIZE], int *legacy) {
  FILE *f = path && *path ? fopen(path, "rb") : NULL;
  if (!f) {
    if (!path || !*path)
      errno = ENOENT;
    return -1;
  }
  char text[ND_PASSWORD_KEY_TEXT + 8u];
  size_t n = fread(text, 1, sizeof(text) - 1u, f);
  int bad = ferror(f) || !feof(f);
  fclose(f);
  text[n] = 0;
  while (n && isspace((unsigned char)text[n - 1]))
    text[--n] = 0;
  int r = bad ? -1 : nd_crypto_parse_password_key(text, salt, key, legacy);
  if (bad)
    errno = EINVAL;
  sodium_memzero(text, sizeof(text));
  return r;
}

void nd_crypto_random(void *buf, size_t len) { randombytes_buf(buf, len); }

static void hash_part(crypto_generichash_state *s, const void *p, size_t n) {
  crypto_generichash_update(s, p, (unsigned long long)n);
}

void nd_crypto_stream_key(uint8_t out[ND_KEY_SIZE],
                          const uint8_t video_key[ND_KEY_SIZE],
                          uint64_t stream_id) {
  crypto_generichash_state s;
  crypto_generichash_init(&s, video_key, ND_KEY_SIZE, ND_KEY_SIZE);
  hash_part(&s, "NetDisplay-v7-video", 19);
  uint64_t wire_id = nd_hton64(stream_id);
  hash_part(&s, &wire_id, sizeof(wire_id));
  crypto_generichash_final(&s, out, ND_KEY_SIZE);
}

int nd_crypto_verify(const uint8_t a[32], const uint8_t b[32]) {
  return sodium_memcmp(a, b, 32) == 0;
}

/* CPace (draft-irtf-cfrg-cpace) on ristretto255. The generator binds the
 * long-term key to this session's nonces. */
static void pake_generator(uint8_t generator[32], const uint8_t key[ND_KEY_SIZE],
                           const struct ndc_hello *hello,
                           const struct ndc_challenge *challenge) {
  static const char domain[] = "NetDisplay-v7-generator";
  uint8_t hash[crypto_hash_sha512_BYTES];
  crypto_hash_sha512_state s;
  crypto_hash_sha512_init(&s);
  crypto_hash_sha512_update(&s, (const uint8_t *)domain, sizeof(domain) - 1u);
  crypto_hash_sha512_update(&s, key, ND_KEY_SIZE);
  crypto_hash_sha512_update(&s, hello->nonce, sizeof(hello->nonce));
  crypto_hash_sha512_update(&s, challenge->nonce, sizeof(challenge->nonce));
  crypto_hash_sha512_final(&s, hash);
  crypto_core_ristretto255_from_hash(generator, hash);
  sodium_memzero(hash, sizeof(hash));
  sodium_memzero(&s, sizeof(s));
}

static int pake_share(struct nd_pake *pake, const uint8_t key[ND_KEY_SIZE],
                      const struct ndc_hello *hello,
                      const struct ndc_challenge *challenge) {
  uint8_t generator[32];
  pake_generator(generator, key, hello, challenge);
  int r = -1;
  /* Only a zero scalar yields the identity; retry rather than send it. */
  for (int attempt = 0; attempt < 4 && r < 0; attempt++) {
    crypto_core_ristretto255_scalar_random(pake->scalar);
    r = crypto_scalarmult_ristretto255(pake->share, pake->scalar, generator);
  }
  sodium_memzero(generator, sizeof(generator));
  if (r < 0)
    errno = EIO;
  return r;
}

/* Everything both sides sent before key confirmation is bound into the
 * session keys, so a changed flag, nonce, salt or share fails the proofs. */
static int pake_keys(const struct nd_pake *pake, const uint8_t key[ND_KEY_SIZE],
                     const uint8_t peer_share[NDC_SHARE_SIZE],
                     const struct ndc_hello *hello,
                     const struct ndc_challenge *challenge,
                     const uint8_t client_share[NDC_SHARE_SIZE],
                     struct nd_session_keys *keys) {
  uint8_t shared[32], transcript[32], master[crypto_kdf_KEYBYTES];
  if (crypto_scalarmult_ristretto255(shared, pake->scalar, peer_share) != 0) {
    errno = EPROTO;
    return -1;
  }
  crypto_generichash_state s;
  crypto_generichash_init(&s, NULL, 0, sizeof(transcript));
  hash_part(&s, "NetDisplay-v7-transcript", 24);
  hash_part(&s, hello, sizeof(*hello));
  hash_part(&s, challenge, sizeof(*challenge));
  hash_part(&s, client_share, NDC_SHARE_SIZE);
  crypto_generichash_final(&s, transcript, sizeof(transcript));

  crypto_generichash_init(&s, shared, sizeof(shared), sizeof(master));
  hash_part(&s, "NetDisplay-v7-session", 21);
  hash_part(&s, transcript, sizeof(transcript));
  hash_part(&s, key, ND_KEY_SIZE);
  crypto_generichash_final(&s, master, sizeof(master));

  static const char context[crypto_kdf_CONTEXTBYTES] = {'N', 'D', 'v', '7',
                                                        'k', 'e', 'y', 's'};
  crypto_kdf_derive_from_key(keys->client_to_server, ND_KEY_SIZE, 1, context,
                             master);
  crypto_kdf_derive_from_key(keys->server_to_client, ND_KEY_SIZE, 2, context,
                             master);
  crypto_kdf_derive_from_key(keys->client_proof, NDC_AUTH_SIZE, 3, context,
                             master);
  crypto_kdf_derive_from_key(keys->server_proof, NDC_AUTH_SIZE, 4, context,
                             master);
  crypto_kdf_derive_from_key(keys->video, ND_KEY_SIZE, 5, context, master);
  sodium_memzero(shared, sizeof(shared));
  sodium_memzero(master, sizeof(master));
  sodium_memzero(&s, sizeof(s));
  return 0;
}

void nd_pake_clear(struct nd_pake *pake) {
  sodium_memzero(pake, sizeof(*pake));
}

int nd_handshake_server_start(struct nd_pake *pake,
                              const uint8_t key[ND_KEY_SIZE],
                              const struct ndc_hello *hello,
                              struct ndc_challenge *challenge) {
  if (pake_share(pake, key, hello, challenge) < 0)
    return -1;
  memcpy(challenge->share, pake->share, sizeof(challenge->share));
  return 0;
}

int nd_handshake_client(const uint8_t key[ND_KEY_SIZE],
                        const struct ndc_hello *hello,
                        const struct ndc_challenge *challenge,
                        struct ndc_auth *auth, struct nd_session_keys *keys) {
  struct nd_pake pake;
  int r = pake_share(&pake, key, hello, challenge);
  if (r == 0)
    r = pake_keys(&pake, key, challenge->share, hello, challenge, pake.share,
                  keys);
  if (r == 0) {
    memcpy(auth->share, pake.share, sizeof(auth->share));
    memcpy(auth->proof, keys->client_proof, sizeof(auth->proof));
  }
  nd_pake_clear(&pake);
  return r;
}

int nd_handshake_server_finish(struct nd_pake *pake,
                               const uint8_t key[ND_KEY_SIZE],
                               const struct ndc_hello *hello,
                               const struct ndc_challenge *challenge,
                               const struct ndc_auth *auth,
                               struct ndc_auth_reply *reply,
                               struct nd_session_keys *keys) {
  if (pake_keys(pake, key, auth->share, hello, challenge, auth->share, keys) < 0)
    return -1;
  if (!nd_crypto_verify(auth->proof, keys->client_proof)) {
    sodium_memzero(keys, sizeof(*keys));
    errno = EACCES;
    return -1;
  }
  memcpy(reply->proof, keys->server_proof, sizeof(reply->proof));
  return 0;
}

static void make_control_nonce(uint8_t nonce[24], uint64_t seq) {
  memset(nonce, 0, 24);
  memcpy(nonce, "NDc7", 4);
  uint64_t wire_seq = nd_hton64(seq);
  memcpy(nonce + 4, &wire_seq, sizeof(wire_seq));
}

static void control_header(uint8_t header[12], uint32_t body_len) {
  struct ndc_hdr h = {
      .magic = htonl(NDC_MAGIC),
      .version = htons(NDC_VERSION),
      .type = htons(NDC_SEALED),
      .length = htonl(body_len),
  };
  _Static_assert(sizeof(h) == 12, "control header must stay 12 bytes");
  memcpy(header, &h, sizeof(h));
}

int nd_crypto_seal_control(uint8_t *out, size_t *out_len, uint16_t type,
                           const void *payload, uint32_t len,
                           const uint8_t key[ND_KEY_SIZE], uint64_t seq) {
  if (len > NDC_MAX_PAYLOAD || (len && !payload)) {
    errno = EMSGSIZE;
    return -1;
  }
  uint8_t plain[2u + NDC_MAX_PAYLOAD], nonce[24];
  uint16_t wire_type = htons(type);
  memcpy(plain, &wire_type, sizeof(wire_type));
  if (len)
    memcpy(plain + 2, payload, len);
  uint32_t body_len = len + NDC_SEAL_OVERHEAD;
  control_header(out, body_len);
  make_control_nonce(nonce, seq);
  unsigned long long n = 0;
  int r = crypto_aead_xchacha20poly1305_ietf_encrypt(
      out + 12, &n, plain, 2u + len, out, 12, NULL, nonce, key);
  sodium_memzero(plain, sizeof(plain));
  if (r || n != body_len) {
    errno = EIO;
    return -1;
  }
  *out_len = 12u + body_len;
  return 0;
}

int nd_crypto_open_control(uint16_t *type, void *payload, uint32_t *len_io,
                           const uint8_t header[12], const uint8_t *body,
                           size_t body_len, const uint8_t key[ND_KEY_SIZE],
                           uint64_t seq) {
  uint8_t expected[12], plain[2u + NDC_MAX_PAYLOAD], nonce[24];
  if (body_len < NDC_SEAL_OVERHEAD || body_len > NDC_MAX_WIRE) {
    errno = EMSGSIZE;
    return -1;
  }
  control_header(expected, (uint32_t)body_len);
  if (memcmp(expected, header, sizeof(expected))) {
    errno = EPROTO;
    return -1;
  }
  make_control_nonce(nonce, seq);
  unsigned long long n = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &n, NULL, body,
                                                 body_len, header, 12, nonce,
                                                 key) != 0 ||
      n < 2u) {
    errno = EBADMSG;
    return -1;
  }
  uint32_t len = (uint32_t)n - 2u;
  if (len > *len_io) {
    sodium_memzero(plain, sizeof(plain));
    errno = EMSGSIZE;
    return -1;
  }
  uint16_t wire_type;
  memcpy(&wire_type, plain, sizeof(wire_type));
  *type = ntohs(wire_type);
  if (len)
    memcpy(payload, plain + 2, len);
  *len_io = len;
  sodium_memzero(plain, sizeof(plain));
  return 0;
}

static void make_nonce(uint8_t nonce[24], uint64_t session, uint32_t seq) {
  memset(nonce, 0, 24);
  memcpy(nonce, "NDv5", 4);
  uint64_t wire_session = nd_hton64(session);
  uint32_t wire_seq = htonl(seq);
  memcpy(nonce + 4, &wire_session, sizeof(wire_session));
  memcpy(nonce + 12, &wire_seq, sizeof(wire_seq));
}

int nd_crypto_encrypt(uint8_t *out, size_t *out_len, const uint8_t *in,
                      size_t in_len, const uint8_t key[ND_KEY_SIZE],
                      uint64_t session, uint32_t seq) {
  uint8_t nonce[24];
  unsigned long long n = 0;
  make_nonce(nonce, session, seq);
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(out, &n, in, in_len, NULL, 0,
                                                 NULL, nonce, key))
    return -1;
  *out_len = (size_t)n;
  return 0;
}

int nd_crypto_decrypt(uint8_t *out, size_t *out_len, const uint8_t *in,
                      size_t in_len, const uint8_t key[ND_KEY_SIZE],
                      uint64_t session, uint32_t seq) {
  uint8_t nonce[24];
  unsigned long long n = 0;
  make_nonce(nonce, session, seq);
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(out, &n, NULL, in, in_len,
                                                 NULL, 0, nonce, key)) {
    errno = EBADMSG;
    return -1;
  }
  *out_len = (size_t)n;
  return 0;
}
