// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_IOS_NATIVE_H
#define ND_IOS_NATIVE_H
#include <stdint.h>
#include <stddef.h>
#include "crypto.h"
#include "control_proto.h"
#include "frame_receiver.h"

typedef struct NDIOSAssembler NDIOSAssembler;
typedef struct NDIOSSession NDIOSSession;
typedef struct {
    uint64_t packets, complete, partial_drop, bad, duplicate, obsolete;
    uint64_t bytes, first_ns, complete_ns;
    uint32_t sequence, frame_bytes, reference, keyframe;
} NDIOSStats;

int nd_ios_init(void);
uint64_t nd_ios_now_ns(void);
void nd_ios_wipe(void *bytes, size_t count);
void nd_ios_hmac(uint8_t out[32], const uint8_t *bytes, size_t count,
                 const uint8_t key[32]);
/* Receiver side of the authenticated handshake. hello and challenge are the
 * exact payloads sent and received. Writes the AUTH payload to auth and
 * returns NULL for a malformed challenge or an invalid source share. */
NDIOSSession *nd_ios_session_create(const uint8_t key[32], const uint8_t *hello,
                                    size_t hello_size, const uint8_t *challenge,
                                    size_t challenge_size,
                                    uint8_t auth[NDC_SHARE_SIZE + NDC_AUTH_SIZE]);
/* 1 when the source's AUTH reply proves it holds the key; every later
 * message is then sealed. */
int nd_ios_session_verify(NDIOSSession *session, const uint8_t *proof,
                          size_t size);
void nd_ios_session_stream_key(const NDIOSSession *session, uint64_t stream_id,
                               uint8_t out[32]);
/* Writes one sealed message, header included, and returns its size, or -1.
 * Messages must be sealed and opened in the order they are sent. */
long nd_ios_session_seal(NDIOSSession *session, uint16_t type,
                         const uint8_t *payload, size_t size, uint8_t *out,
                         size_t capacity);
/* Opens the body that followed a sealed header. Returns the payload size,
 * or -1 when the message is not authentic or out of order. */
long nd_ios_session_open(NDIOSSession *session, const uint8_t header[12],
                         const uint8_t *body, size_t body_size, uint16_t *type,
                         uint8_t *payload, size_t capacity);
void nd_ios_session_destroy(NDIOSSession *session);
NDIOSAssembler *nd_ios_assembler_create(uint64_t session, const uint8_t *key);
void nd_ios_assembler_destroy(NDIOSAssembler *assembler);
/* Exactly one input datagram. 1 = complete frame, 0 = partial/discard,
 * -1 = malformed/authentication failure. Never accepts another session. */
int nd_ios_assembler_feed(NDIOSAssembler *assembler, const uint8_t *packet,
                           size_t size);
/* Drain any additional frame immediately after feed. */
int nd_ios_assembler_next(NDIOSAssembler *assembler);
/* Valid until the next feed or drain. Copy/consume before returning to the RX loop. */
const uint8_t *nd_ios_assembler_bytes(const NDIOSAssembler *assembler);
NDIOSStats nd_ios_assembler_stats(const NDIOSAssembler *assembler);
#endif
