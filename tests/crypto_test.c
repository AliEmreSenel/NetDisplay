// SPDX-License-Identifier: GPL-2.0-only
#include "crypto.h"

#include <errno.h>
#include <sodium/core.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  uint8_t psk[ND_KEY_SIZE];
  uint8_t key1[ND_KEY_SIZE], key2[ND_KEY_SIZE];
  for (unsigned i = 0; i < sizeof(psk); i++)
    psk[i] = (uint8_t)i;
  if (sodium_init() < 0)
    return 1;
  nd_crypto_stream_key(key1, psk, 42);
  nd_crypto_stream_key(key2, psk, 42);
  if (memcmp(key1, key2, sizeof(key1)))
    return 2;
  nd_crypto_stream_key(key2, psk, 43);
  if (!memcmp(key1, key2, sizeof(key1)))
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
  uint8_t salt[ND_SALT_SIZE] = {1}, other_salt[ND_SALT_SIZE] = {2};
  if (nd_crypto_password_key(password_key1, "correct horse battery staple",
                             salt) < 0 ||
      nd_crypto_password_key(password_key2, "correct horse battery staple",
                             salt) < 0 ||
      memcmp(password_key1, password_key2, sizeof(password_key1)))
    return 12;
  if (nd_crypto_password_key(password_key2, "different password", salt) < 0 ||
      !memcmp(password_key1, password_key2, sizeof(password_key1)))
    return 13;
  /* The same password on another install gives another key. */
  if (nd_crypto_password_key(password_key2, "correct horse battery staple",
                             other_salt) < 0 ||
      !memcmp(password_key1, password_key2, sizeof(password_key1)))
    return 16;
  char stored[ND_PASSWORD_KEY_TEXT];
  uint8_t parsed_salt[ND_SALT_SIZE], parsed_key[ND_KEY_SIZE];
  int legacy = -1;
  nd_crypto_format_password_key(stored, salt, password_key1);
  if (strlen(stored) != ND_PASSWORD_KEY_TEXT - 1u || stored[32] != ':' ||
      nd_crypto_parse_password_key(stored, parsed_salt, parsed_key, &legacy) < 0 ||
      legacy || memcmp(parsed_salt, salt, sizeof(salt)) ||
      memcmp(parsed_key, password_key1, sizeof(parsed_key)))
    return 17;
  /* A bare key from an older --derive-password-key keeps working. */
  char legacy_hex[ND_KEY_SIZE * 2u + 1u];
  nd_crypto_key_to_hex(legacy_hex, password_key1);
  if (nd_crypto_parse_password_key(legacy_hex, parsed_salt, parsed_key,
                                   &legacy) < 0 || legacy != 1 ||
      memcmp(parsed_salt, nd_crypto_legacy_salt, sizeof(parsed_salt)) ||
      memcmp(parsed_key, password_key1, sizeof(parsed_key)))
    return 18;
  stored[5] = ':';
  if (nd_crypto_parse_password_key(stored, parsed_salt, parsed_key, NULL) == 0)
    return 19;

  /* Sealed control messages: round trip, sequence binding and tampering. */
  uint8_t wire[12 + NDC_MAX_WIRE], payload[NDC_MAX_PAYLOAD];
  size_t wire_len = 0;
  uint32_t payload_len = sizeof(payload);
  uint16_t type = 0;
  if (nd_crypto_seal_control(wire, &wire_len, NDC_INPUT, "key", 3, key1, 5) < 0 ||
      wire_len != 12u + 3u + NDC_SEAL_OVERHEAD ||
      nd_crypto_open_control(&type, payload, &payload_len, wire, wire + 12,
                             wire_len - 12u, key1, 5) < 0 ||
      type != NDC_INPUT || payload_len != 3 || memcmp(payload, "key", 3))
    return 20;
  payload_len = sizeof(payload);
  if (nd_crypto_open_control(&type, payload, &payload_len, wire, wire + 12,
                             wire_len - 12u, key1, 6) == 0)
    return 21;
  wire[3] ^= 1;
  if (nd_crypto_open_control(&type, payload, &payload_len, wire, wire + 12,
                             wire_len - 12u, key1, 5) == 0)
    return 22;
  wire[3] ^= 1;
  payload_len = 2;
  if (nd_crypto_open_control(&type, payload, &payload_len, wire, wire + 12,
                             wire_len - 12u, key1, 5) == 0 || errno != EMSGSIZE)
    return 23;
  if (nd_crypto_seal_control(wire, &wire_len, NDC_INPUT, payload,
                             NDC_MAX_PAYLOAD + 1u, key1, 0) == 0)
    return 24;
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
