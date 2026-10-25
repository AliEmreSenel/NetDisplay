// SPDX-License-Identifier: GPL-2.0-only
#include "NDNative.h"
#include "frame_receiver.h"
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

_Static_assert(NDC_VERSION == 6, "Re-audit iOS control protocol for the new version");
_Static_assert(sizeof(struct ndc_hello) == 28, "Unexpected HELLO layout");
_Static_assert(sizeof(struct ndc_display) == 76, "Unexpected DISPLAY layout");
_Static_assert(sizeof(struct ndc_stream) == 84, "Unexpected STREAM layout");

struct NDIOSAssembler {
    struct nd_assembly assembly;
    struct nd_rx_stats rx;
    uint8_t *buffer;
    uint8_t key[ND_KEY_SIZE];
    int encrypted;
    NDIOSStats stats;
};
int nd_ios_init(void) { return sodium_init(); }
uint64_t nd_ios_now_ns(void) { return nd_now_ns(); }
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
    a->assembly.expected_session = session;
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
    uint64_t bad = atomic_load(&a->rx.bad);
    int result = nd_frame_receive(&a->assembly, &a->rx, a->buffer, p, n,
                                  a->encrypted ? a->key : NULL);
    a->stats.complete = atomic_load(&a->rx.complete);
    a->stats.partial_drop = atomic_load(&a->rx.partial_drop);
    a->stats.bad = atomic_load(&a->rx.bad);
    a->stats.duplicate = atomic_load(&a->rx.dup);
    a->stats.obsolete = atomic_load(&a->rx.obsolete);
    if (result == 1) {
        a->stats.sequence = a->assembly.seq;
        a->stats.frame_bytes = a->assembly.size;
        a->stats.first_ns = a->assembly.first_ns;
        a->stats.complete_ns = a->assembly.complete_ns;
    }
    return a->stats.bad != bad ? -1 : result;
}

const uint8_t *nd_ios_assembler_bytes(const NDIOSAssembler *a) {
    return a ? a->buffer : NULL;
}
NDIOSStats nd_ios_assembler_stats(const NDIOSAssembler *a) {
    return a ? a->stats : (NDIOSStats){0};
}
