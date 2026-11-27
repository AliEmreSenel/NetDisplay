// SPDX-License-Identifier: GPL-2.0-only
#include "NDNative.h"
#include "frame_receiver.h"
#include <stdlib.h>
#include <string.h>
#include <sodium.h>

_Static_assert(NDC_VERSION == 7, "Re-audit iOS control protocol for the new version");
_Static_assert(sizeof(struct ndc_hello) == 28, "Unexpected HELLO layout");
_Static_assert(sizeof(struct ndc_challenge) == 68, "Unexpected CHALLENGE layout");
_Static_assert(sizeof(struct ndc_auth) == 64, "Unexpected AUTH layout");
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
struct NDIOSSession {
    struct nd_session_keys keys;
    uint64_t tx_seq, rx_seq;
    int sealed;
};

int nd_ios_init(void) { return sodium_init(); }

NDIOSSession *nd_ios_session_create(const uint8_t key[32], const uint8_t *hello,
                                    size_t hello_size, const uint8_t *challenge,
                                    size_t challenge_size,
                                    uint8_t auth[NDC_SHARE_SIZE + NDC_AUTH_SIZE]) {
    struct ndc_hello h;
    struct ndc_challenge c;
    struct ndc_auth a;
    if (!key || !hello || !challenge || !auth || hello_size != sizeof(h) ||
        challenge_size != sizeof(c) || nd_ios_init() < 0) return NULL;
    memcpy(&h, hello, sizeof(h));
    memcpy(&c, challenge, sizeof(c));
    NDIOSSession *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (nd_handshake_client(key, &h, &c, &a, &s->keys) < 0) {
        nd_ios_session_destroy(s);
        return NULL;
    }
    memcpy(auth, &a, sizeof(a));
    return s;
}

int nd_ios_session_verify(NDIOSSession *s, const uint8_t *proof, size_t size) {
    if (!s || !proof || size != NDC_AUTH_SIZE || s->sealed ||
        !nd_crypto_verify(proof, s->keys.server_proof)) return 0;
    s->sealed = 1;
    return 1;
}

void nd_ios_session_stream_key(const NDIOSSession *s, uint64_t stream_id,
                               uint8_t out[32]) {
    if (s) nd_crypto_stream_key(out, s->keys.video, stream_id);
}

long nd_ios_session_seal(NDIOSSession *s, uint16_t type, const uint8_t *payload,
                         size_t size, uint8_t *out, size_t capacity) {
    size_t n = 0;
    if (!s || !s->sealed || !out || size > NDC_MAX_PAYLOAD ||
        capacity < sizeof(struct ndc_hdr) + size + NDC_SEAL_OVERHEAD ||
        nd_crypto_seal_control(out, &n, type, payload, (uint32_t)size,
                               s->keys.client_to_server, s->tx_seq) < 0)
        return -1;
    s->tx_seq++;
    return (long)n;
}

long nd_ios_session_open(NDIOSSession *s, const uint8_t header[12],
                         const uint8_t *body, size_t body_size, uint16_t *type,
                         uint8_t *payload, size_t capacity) {
    uint32_t n = capacity > NDC_MAX_PAYLOAD ? NDC_MAX_PAYLOAD : (uint32_t)capacity;
    if (!s || !s->sealed || !header || !body || !type || !payload ||
        nd_crypto_open_control(type, payload, &n, header, body, body_size,
                               s->keys.server_to_client, s->rx_seq) < 0)
        return -1;
    s->rx_seq++;
    return (long)n;
}

void nd_ios_session_destroy(NDIOSSession *s) {
    if (!s) return;
    nd_ios_wipe(s, sizeof(*s));
    free(s);
}
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
