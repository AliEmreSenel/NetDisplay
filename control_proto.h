#ifndef NETDISPLAY_CONTROL_PROTO_H
#define NETDISPLAY_CONTROL_PROTO_H

#include <stdint.h>

#define NDC_MAGIC 0x4e444333u /* NDC3 */
#define NDC_DISC_MAGIC 0x4e444344u /* NDCD */
#define NDC_VERSION 3u
#define NDC_DEFAULT_PORT 5001u
#define NDC_DEFAULT_VIDEO_PORT 5000u
#define NDC_MAX_PAYLOAD 256u

#define NDC_FLAG_WANT_INPUT    (1u << 0)
#define NDC_FLAG_INPUT_ALLOWED (1u << 0)

#define NDC_DEV_KBM      1u
#define NDC_DEV_TOUCHPAD 2u

enum ndc_type {
    NDC_HELLO   = 1,
    NDC_WELCOME = 2,
    NDC_INPUT   = 3,
    NDC_PING    = 4,
    NDC_PONG    = 5,
    NDC_STOP    = 6,
    NDC_READY   = 7,
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

/* Receiver -> source. Fixed capability request only. No command/string field. */
struct __attribute__((packed)) ndc_hello {
    uint32_t flags;
};

/* Source -> receiver. Source chooses the actual stream mode. */
struct __attribute__((packed)) ndc_welcome {
    uint32_t flags;
    uint16_t video_port;
    uint16_t width;
    uint16_t height;
    uint16_t refresh_hz;
};

struct __attribute__((packed)) ndc_input {
    uint8_t device;
    uint8_t reserved0;
    uint16_t type;
    uint16_t code;
    uint16_t reserved1;
    int32_t value;
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

_Static_assert(sizeof(struct ndc_hdr) == 12, "ndc_hdr wire size");
_Static_assert(sizeof(struct ndc_input) == 12, "ndc_input wire size");
_Static_assert(sizeof(struct ndc_welcome) == 12, "ndc_welcome wire size");
_Static_assert(sizeof(struct ndc_discovery) == 16, "discovery wire size");

#endif
