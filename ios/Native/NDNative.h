// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_IOS_NATIVE_H
#define ND_IOS_NATIVE_H
#include <stdint.h>
#include <stddef.h>
#include "crypto.h"
#include "control_proto.h"

typedef struct NDIOSAssembler NDIOSAssembler;
typedef struct {
    uint64_t packets, complete, partial_drop, bad, duplicate, obsolete;
    uint64_t bytes, first_ns, complete_ns;
    uint32_t sequence, frame_bytes;
} NDIOSStats;

int nd_ios_init(void);
uint64_t nd_ios_now_ns(void);
void nd_ios_wipe(void *bytes, size_t count);
void nd_ios_hmac(uint8_t out[32], const uint8_t *bytes, size_t count,
                 const uint8_t key[32]);
NDIOSAssembler *nd_ios_assembler_create(uint64_t session, const uint8_t *key);
void nd_ios_assembler_destroy(NDIOSAssembler *assembler);
/* Exactly one input datagram. 1 = complete frame, 0 = partial/discard,
 * -1 = malformed/authentication failure. Never accepts another session. */
int nd_ios_assembler_feed(NDIOSAssembler *assembler, const uint8_t *packet,
                           size_t size);
/* Valid until the next feed. Copy/consume before returning to the RX loop. */
const uint8_t *nd_ios_assembler_bytes(const NDIOSAssembler *assembler);
NDIOSStats nd_ios_assembler_stats(const NDIOSAssembler *assembler);
#endif
