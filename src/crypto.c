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

int nd_crypto_key_from_hex(uint8_t key[ND_KEY_SIZE], const char *hex) {
  if (!hex || strlen(hex) != ND_KEY_SIZE * 2u) {
    errno = EINVAL;
    return -1;
  }
  for (size_t i = 0; i < ND_KEY_SIZE; i++) {
    int hi = hex_value((unsigned char)hex[2u * i]);
    int lo = hex_value((unsigned char)hex[2u * i + 1u]);
    if (hi < 0 || lo < 0) {
      sodium_memzero(key, ND_KEY_SIZE);
      errno = EINVAL;
      return -1;
    }
    key[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
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

int nd_crypto_password_key(uint8_t key[ND_KEY_SIZE], const char *password) {
  static const uint8_t salt[crypto_pwhash_SALTBYTES] = {
      'N', 'e', 't', 'D', 'i', 's', 'p', 'l',
      'a', 'y', 'P', 'a', 's', 's', 'v', '1'};
  if (!password || !*password) {
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

void nd_crypto_random(void *buf, size_t len) { randombytes_buf(buf, len); }

static void hash_part(crypto_generichash_state *s, const void *p, size_t n) {
  crypto_generichash_update(s, p, (unsigned long long)n);
}

void nd_crypto_proof(uint8_t out[32], const uint8_t key[ND_KEY_SIZE],
                     const char *role, const uint8_t cn[16],
                     const uint8_t sn[16]) {
  crypto_generichash_state s;
  crypto_generichash_init(&s, key, ND_KEY_SIZE, 32);
  hash_part(&s, "NetDisplay-v5-auth", 18);
  hash_part(&s, role, strlen(role));
  hash_part(&s, cn, 16);
  hash_part(&s, sn, 16);
  crypto_generichash_final(&s, out, 32);
}

void nd_crypto_stream_key(uint8_t out[ND_KEY_SIZE],
                          const uint8_t psk[ND_KEY_SIZE], uint64_t stream_id,
                          const uint8_t cn[16], const uint8_t sn[16]) {
  crypto_generichash_state s;
  crypto_generichash_init(&s, psk, ND_KEY_SIZE, ND_KEY_SIZE);
  hash_part(&s, "NetDisplay-v5-video", 19);
  hash_part(&s, cn, 16);
  hash_part(&s, sn, 16);
  uint64_t wire_id = nd_hton64(stream_id);
  hash_part(&s, &wire_id, sizeof(wire_id));
  crypto_generichash_final(&s, out, ND_KEY_SIZE);
}

int nd_crypto_verify(const uint8_t a[32], const uint8_t b[32]) {
  return sodium_memcmp(a, b, 32) == 0;
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
