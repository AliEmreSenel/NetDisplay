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
    struct nd_reorder reorder;
    struct nd_reorder_slot *output;
    struct nd_rx_stats rx;
    uint8_t *buffer, *extra;
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
    a->extra = malloc(ND_MAX_WIRE_FRAME);
    if (!a->extra) { free(a->buffer); free(a); return NULL; }
    nd_reorder_init(&a->reorder, session, a->buffer, a->extra);
    if (key) { a->encrypted = 1; memcpy(a->key, key, 32); }
    return a;
}
void nd_ios_assembler_destroy(NDIOSAssembler *a) {
    if (!a) return;
    nd_ios_wipe(a->key, sizeof(a->key));
    free(a->extra);
    free(a->buffer);
    free(a);
}
int nd_ios_assembler_feed(NDIOSAssembler *a, const uint8_t *p, size_t n) {
    if (!a || !p) return -1;
    a->stats.packets++;
    a->stats.bytes += n;
    uint64_t bad = atomic_load(&a->rx.bad);
    nd_reorder_feed(&a->reorder, &a->rx, p, n, a->encrypted ? a->key : NULL, nd_now_ns());
    int result = nd_ios_assembler_next(a);
    a->stats.complete = atomic_load(&a->rx.complete);
    a->stats.partial_drop = atomic_load(&a->rx.partial_drop);
    a->stats.bad = atomic_load(&a->rx.bad);
    a->stats.duplicate = atomic_load(&a->rx.dup);
    a->stats.obsolete = atomic_load(&a->rx.obsolete);

    return a->stats.bad != bad ? -1 : result;
}

int nd_ios_assembler_next(NDIOSAssembler *a) {
    if (!a) return 0;
    a->output = nd_reorder_take(&a->reorder);
    if (!a->output) return 0;
    struct nd_assembly *frame = &a->output->assembly;
    a->stats.sequence = frame->seq;
    a->stats.frame_bytes = frame->size;
    a->stats.reference = frame->reference;
    a->stats.keyframe = (uint32_t)frame->keyframe;
    a->stats.first_ns = frame->first_ns;
    a->stats.complete_ns = frame->complete_ns;
    return 1;
}

const uint8_t *nd_ios_assembler_bytes(const NDIOSAssembler *a) {
    return a && a->output ? a->output->buffer + a->output->assembly.offset : NULL;
}
NDIOSStats nd_ios_assembler_stats(const NDIOSAssembler *a) {
    return a ? a->stats : (NDIOSStats){0};
}
