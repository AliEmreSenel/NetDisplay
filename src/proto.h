#ifndef NETDISPLAY_PROTO_H
#define NETDISPLAY_PROTO_H

#include <arpa/inet.h>
#include <stdint.h>

#define ND_MAGIC 0x4e443031u /* "ND01" */
#define ND_PORT 5000
#define ND_DEFAULT_W 1920u
#define ND_DEFAULT_H 1080u
#define ND_DEFAULT_FPS 60u
#define ND_UDP_PAYLOAD_MAX 1472u
#define ND_MAX_FRAME (4u * 1024u * 1024u)
#define ND_MAX_WIRE_FRAME (ND_MAX_FRAME + 16u)

struct __attribute__((packed)) nd_hdr {
    uint32_t magic;
    uint64_t session;
    uint32_t seq;
    uint16_t frag;
    uint16_t frag_count;
    uint32_t frame_size;
};

#define ND_HDR_SIZE ((unsigned)sizeof(struct nd_hdr))
#define ND_FRAG_DATA (ND_UDP_PAYLOAD_MAX - ND_HDR_SIZE)
#define ND_MAX_FRAGS ((ND_MAX_WIRE_FRAME + ND_FRAG_DATA - 1u) / ND_FRAG_DATA)

_Static_assert(sizeof(struct nd_hdr) == 24, "wire header must stay 24 bytes");
_Static_assert(ND_FRAG_DATA > 1000, "fragment payload unexpectedly small");
_Static_assert(ND_MAX_FRAGS < 65536, "frag_count no longer fits uint16_t");

static inline uint64_t nd_bswap64(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(x);
#else
    return ((x & 0x00000000000000ffULL) << 56) |
           ((x & 0x000000000000ff00ULL) << 40) |
           ((x & 0x0000000000ff0000ULL) << 24) |
           ((x & 0x00000000ff000000ULL) << 8)  |
           ((x & 0x000000ff00000000ULL) >> 8)  |
           ((x & 0x0000ff0000000000ULL) >> 24) |
           ((x & 0x00ff000000000000ULL) >> 40) |
           ((x & 0xff00000000000000ULL) >> 56);
#endif
}

static inline uint64_t nd_hton64(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return nd_bswap64(x);
#else
    return x;
#endif
}

static inline uint64_t nd_ntoh64(uint64_t x)
{
    return nd_hton64(x);
}

#endif
