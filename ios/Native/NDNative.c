// SPDX-License-Identifier: GPL-2.0-only
#include "NDNative.h"
#include "proto.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sodium.h>

_Static_assert(NDC_VERSION == 6, "Re-audit iOS control protocol for the new version");
_Static_assert(sizeof(struct ndc_hello) == 28, "Unexpected HELLO layout");
_Static_assert(sizeof(struct ndc_display) == 76, "Unexpected DISPLAY layout");
_Static_assert(sizeof(struct ndc_stream) == 84, "Unexpected STREAM layout");

struct NDIOSAssembler {
    uint64_t session;
    uint8_t *buffer;
    uint8_t key[32];
    uint64_t got[(ND_MAX_FRAGS + 63u) / 64u];
    uint32_t seq, size, received;
    uint16_t count;
    int have_sequence, active, encrypted;
    NDIOSStats stats;
};
int nd_ios_init(void) { return sodium_init(); }
uint64_t nd_ios_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void nd_ios_wipe(void *p, size_t n) { sodium_memzero(p, n); }
void nd_ios_hmac(uint8_t out[32], const uint8_t *p, size_t n, const uint8_t key[32]) {
    crypto_auth_hmacsha256(out, p, (unsigned long long)n, key);
}
NDIOSAssembler *nd_ios_assembler_create(uint64_t session, const uint8_t *key) {
    if (!session || nd_ios_init() < 0) return NULL;
    NDIOSAssembler *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->buffer = malloc(ND_MAX_WIRE_FRAME);
    if (!a->buffer) { free(a); return NULL; }
    a->session = session;
    if (key) { a->encrypted = 1; memcpy(a->key, key, 32); }
    return a;
}
void nd_ios_assembler_destroy(NDIOSAssembler *a) {
    if (!a) return;
    nd_ios_wipe(a->key, sizeof(a->key));
    free(a->buffer);
    free(a);
}
int nd_ios_assembler_feed(NDIOSAssembler *a, const uint8_t *p, size_t n) {
    if (!a || !p) return -1;
    a->stats.packets++;
    a->stats.bytes += n;
    struct nd_hdr h;
    if (n < sizeof(h) || n > ND_UDP_PAYLOAD_MAX) goto bad;
    memcpy(&h, p, sizeof(h));
    uint64_t session = nd_ntoh64(h.session);
    uint32_t seq = ntohl(h.seq), size = ntohl(h.frame_size);
    uint16_t frag = ntohs(h.frag), count = ntohs(h.frag_count);
    if (ntohl(h.magic) != ND_MAGIC || !session || !size ||
        size > (a->encrypted ? ND_MAX_WIRE_FRAME : ND_MAX_FRAME) || !count ||
        count != (size + ND_FRAG_DATA - 1u) / ND_FRAG_DATA || frag >= count)
        goto bad;
    if (session != a->session) { a->stats.obsolete++; return 0; }
    size_t offset = (size_t)frag * ND_FRAG_DATA;
    size_t wanted = size - offset;
    if (wanted > ND_FRAG_DATA) wanted = ND_FRAG_DATA;
    if (n != sizeof(h) + wanted) goto bad;
    if (!a->have_sequence || (int32_t)(seq - a->seq) > 0) {
        if (a->active) a->stats.partial_drop++;
        a->have_sequence = 1;
        a->seq = seq; a->size = size; a->count = count;
        a->received = 0; a->active = 1;
        a->stats.first_ns = nd_ios_now_ns();
        memset(a->got, 0, sizeof(a->got));
    } else if (!a->active || seq != a->seq) {
        a->stats.obsolete++; return 0;
    } else if (size != a->size || count != a->count) goto bad;
    uint64_t bit = 1ull << (frag & 63u);
    if (a->got[frag >> 6] & bit) { a->stats.duplicate++; return 0; }
    memcpy(a->buffer + offset, p + sizeof(h), wanted);
    a->got[frag >> 6] |= bit;
    if (++a->received != a->count) return 0;
    a->active = 0;
    size_t plain = a->size;
    if (a->encrypted &&
        (nd_crypto_decrypt(a->buffer, &plain, a->buffer, a->size, a->key,
                           a->session, a->seq) < 0 || !plain || plain > ND_MAX_FRAME))
        goto bad;
    a->stats.complete++;
    a->stats.sequence = a->seq;
    a->stats.frame_bytes = (uint32_t)plain;
    a->stats.complete_ns = nd_ios_now_ns();
    return 1;
bad:
    a->stats.bad++;
    return -1;
}
const uint8_t *nd_ios_assembler_bytes(const NDIOSAssembler *a) {
    return a ? a->buffer : NULL;
}
NDIOSStats nd_ios_assembler_stats(const NDIOSAssembler *a) {
    return a ? a->stats : (NDIOSStats){0};
}
