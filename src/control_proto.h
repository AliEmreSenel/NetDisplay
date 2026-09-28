// SPDX-License-Identifier: GPL-2.0-only
#ifndef NETDISPLAY_CONTROL_PROTO_H
#define NETDISPLAY_CONTROL_PROTO_H

#include <stdint.h>

#define NDC_MAGIC 0x4e444333u      /* NDC3 */
#define NDC_DISC_MAGIC 0x4e444344u /* NDCD */
#define NDC_VERSION 5u
#define NDC_DEFAULT_PORT 5001u
#define NDC_DEFAULT_VIDEO_PORT 5000u
#define NDC_MAX_PAYLOAD 512u
#define NDC_MAX_DISPLAYS 8u
#define NDC_NAME_MAX 64u
#define NDC_NONCE_SIZE 16u
#define NDC_AUTH_SIZE 32u

#define NDC_FLAG_WANT_INPUT (1u << 0)
#define NDC_FLAG_INPUT_ALLOWED (1u << 0)
#define NDC_FLAG_FRAME_ENCRYPT (1u << 1)
#define NDC_FLAG_AUTH_REQUIRED (1u << 2)
#define NDC_FLAG_PASSWORD (1u << 3)
#define NDC_FLAG_NETWORK_TEST (1u << 4)
#define NDC_FLAG_POWER_INFO (1u << 5)
/* Compatibility name for protocol-v5 implementations predating passwords. */
#define NDC_FLAG_HAVE_PSK NDC_FLAG_AUTH_REQUIRED

#define NDC_DISPLAY_HAS_DPMS (1u << 0)
#define NDC_DISPLAY_HAS_BRIGHTNESS (1u << 1)
#define NDC_DISPLAY_DPMS_ON (1u << 2)

#define NDC_DEV_KBM 1u
#define NDC_DEV_TOUCHPAD 2u

enum ndc_type {
  NDC_HELLO = 1,
  NDC_WELCOME = 2,
  NDC_INPUT = 3,
  NDC_PING = 4,
  NDC_PONG = 5,
  NDC_STOP = 6,
  NDC_READY = 7,
  NDC_DISPLAY_STATE = 8,
  NDC_CHALLENGE = 9,
  NDC_AUTH = 10,
  NDC_DISPLAY = 11,
  NDC_STREAM = 12,
  NDC_STREAM_READY = 13,
  NDC_REJECT = 14,
  NDC_TEST_PORT = 15,
  NDC_TEST_READY = 16,
  NDC_TEST_DONE = 17,
  NDC_TEST_REPORT = 18,
  NDC_POWER_BEGIN = 19,
  NDC_POWER_DEVICE = 20,
  NDC_POWER_PROPERTY = 21,
  NDC_POWER_END = 22,
};

enum ndc_discovery_type {
  NDC_DISCOVER = 1,
  NDC_OFFER = 2,
};

struct __attribute__((packed)) ndc_hdr {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t length;
};

/* Receiver -> source. Fixed capability request only. No command/string field.
 */
struct __attribute__((packed)) ndc_hello {
  uint32_t flags;
  uint16_t display_count;
  uint16_t reserved;
  uint8_t nonce[NDC_NONCE_SIZE];
};

struct __attribute__((packed)) ndc_challenge {
  uint32_t flags;
  uint8_t nonce[NDC_NONCE_SIZE];
};

struct __attribute__((packed)) ndc_auth {
  uint8_t proof[NDC_AUTH_SIZE];
};

struct __attribute__((packed)) ndc_display {
  uint32_t display_id;
  uint16_t width;
  uint16_t height;
  uint16_t refresh_hz;
  uint16_t reserved;
  char connector[NDC_NAME_MAX];
};

/* Source -> receiver. One follows for every advertised display. */
struct __attribute__((packed)) ndc_stream {
  uint32_t display_id;
  uint64_t stream_id;
  uint16_t video_port;
  uint16_t width;
  uint16_t height;
  uint16_t refresh_hz;
  char output[NDC_NAME_MAX];
};

struct __attribute__((packed)) ndc_stream_ready {
  uint32_t display_id;
};

/* Source -> receiver. Session-wide negotiated capabilities. */
struct __attribute__((packed)) ndc_welcome {
  uint32_t flags;
  uint16_t display_count;
  uint16_t reserved;
};

struct __attribute__((packed)) ndc_input {
  uint8_t device;
  uint8_t reserved0;
  uint16_t type;
  uint16_t code;
  uint16_t reserved1;
  int32_t value;
};

/* Source -> receiver. Brightness is expressed as hundredths of one percent,
 * so 0 is dark and 10000 is the source panel's maximum. */
struct __attribute__((packed)) ndc_display_state {
  uint32_t flags;
  uint32_t brightness;
};

/* UDP discovery on the control port. A receiver broadcasts DISCOVER and a
 * source replies OFFER to the packet source address. TCP then carries the
 * actual control session. */
struct __attribute__((packed)) ndc_discovery {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t nonce;
  uint16_t tcp_port;
  uint16_t reserved;
};

#endif
