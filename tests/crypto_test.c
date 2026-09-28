// SPDX-License-Identifier: GPL-2.0-only
#include "crypto.h"

#include <sodium/core.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  uint8_t psk[ND_KEY_SIZE], client_nonce[16], server_nonce[16];
  uint8_t key1[ND_KEY_SIZE], key2[ND_KEY_SIZE], proof1[32], proof2[32];
  for (unsigned i = 0; i < sizeof(psk); i++)
    psk[i] = (uint8_t)i;
  for (unsigned i = 0; i < sizeof(client_nonce); i++) {
    client_nonce[i] = (uint8_t)(i + 32u);
    server_nonce[i] = (uint8_t)(i + 64u);
  }
  if (sodium_init() < 0)
    return 1;
  nd_crypto_stream_key(key1, psk, 42, client_nonce, server_nonce);
  nd_crypto_stream_key(key2, psk, 42, client_nonce, server_nonce);
  if (memcmp(key1, key2, sizeof(key1)))
    return 2;
  nd_crypto_proof(proof1, psk, "client", client_nonce, server_nonce);
  nd_crypto_proof(proof2, psk, "client", client_nonce, server_nonce);
  if (!nd_crypto_verify(proof1, proof2))
    return 3;

  static const uint8_t plain[] = "independent encoded frame";
  uint8_t cipher[sizeof(plain) + ND_TAG_SIZE], decoded[sizeof(plain)];
  size_t cipher_size = 0, decoded_size = 0;
  if (nd_crypto_encrypt(cipher, &cipher_size, plain, sizeof(plain), key1, 42,
                        7) < 0)
    return 4;
  if (cipher_size != sizeof(plain) + ND_TAG_SIZE)
    return 5;
  if (nd_crypto_decrypt(decoded, &decoded_size, cipher, cipher_size, key1, 42,
                        7) < 0)
    return 6;
  if (decoded_size != sizeof(plain) || memcmp(decoded, plain, sizeof(plain)))
    return 7;
  if (nd_crypto_encrypt(cipher, &cipher_size, plain, sizeof(plain), key1, 42,
                        8) < 0)
    return 8;
  if (nd_crypto_decrypt(cipher, &decoded_size, cipher, cipher_size, key1, 42,
                        8) < 0 ||
      decoded_size != sizeof(plain) || memcmp(cipher, plain, sizeof(plain)))
    return 9;
  if (nd_crypto_encrypt(cipher, &cipher_size, plain, sizeof(plain), key1, 42,
                        9) < 0)
    return 10;
  cipher[0] ^= 1;
  if (nd_crypto_decrypt(decoded, &decoded_size, cipher, cipher_size, key1, 42,
                        9) == 0)
    return 11;
  uint8_t password_key1[ND_KEY_SIZE], password_key2[ND_KEY_SIZE];
  if (nd_crypto_password_key(password_key1, "correct horse battery staple") <
          0 ||
      nd_crypto_password_key(password_key2, "correct horse battery staple") <
          0 ||
      memcmp(password_key1, password_key2, sizeof(password_key1)))
    return 12;
  if (nd_crypto_password_key(password_key2, "different password") < 0 ||
      !memcmp(password_key1, password_key2, sizeof(password_key1)))
    return 13;
  char password_hex[ND_KEY_SIZE * 2u + 1u];
  nd_crypto_key_to_hex(password_hex, password_key1);
  memset(password_key2, 0, sizeof(password_key2));
  if (nd_crypto_key_from_hex(password_key2, password_hex) < 0 ||
      memcmp(password_key1, password_key2, sizeof(password_key1)))
    return 14;
  password_hex[0] = 'z';
  if (nd_crypto_key_from_hex(password_key2, password_hex) == 0)
    return 15;
  puts("crypto roundtrip and tamper rejection passed");
  return 0;
}
