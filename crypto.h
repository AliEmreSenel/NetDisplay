#ifndef NETDISPLAY_CRYPTO_H
#define NETDISPLAY_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define ND_KEY_SIZE 32u
#define ND_TAG_SIZE 16u

int nd_crypto_init(void);
int nd_crypto_load_psk(const char *path, uint8_t key[ND_KEY_SIZE]);
int nd_crypto_password_key(uint8_t key[ND_KEY_SIZE], const char *password);
int nd_crypto_key_from_hex(uint8_t key[ND_KEY_SIZE], const char *hex);
void nd_crypto_key_to_hex(char hex[ND_KEY_SIZE * 2u + 1u],
                          const uint8_t key[ND_KEY_SIZE]);
void nd_crypto_random(void *buf, size_t len);
void nd_crypto_proof(uint8_t out[32], const uint8_t key[ND_KEY_SIZE],
                     const char *role, const uint8_t client_nonce[16],
                     const uint8_t server_nonce[16]);
void nd_crypto_stream_key(uint8_t out[ND_KEY_SIZE],
                          const uint8_t psk[ND_KEY_SIZE], uint64_t stream_id,
                          const uint8_t client_nonce[16],
                          const uint8_t server_nonce[16]);
int nd_crypto_verify(const uint8_t a[32], const uint8_t b[32]);
int nd_crypto_encrypt(uint8_t *out, size_t *out_len, const uint8_t *in,
                      size_t in_len, const uint8_t key[ND_KEY_SIZE],
                      uint64_t session, uint32_t seq);
int nd_crypto_decrypt(uint8_t *out, size_t *out_len, const uint8_t *in,
                      size_t in_len, const uint8_t key[ND_KEY_SIZE],
                      uint64_t session, uint32_t seq);

#endif
