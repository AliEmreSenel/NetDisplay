// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_NETWORK_TEST_H
#define ND_NETWORK_TEST_H
#include "control_proto.h"
/* A video port of zero exchanges ephemeral UDP ports for a standalone test.
 * A positive video port uses the legacy probe before video workers start.
 * Returns -1 on setup/control failure; UDP loss is a reported test result. */
int nd_network_test(int control, int server, const char *peer, int video_port,
                     const char *interface_name, int fps, uint64_t session,
                     const uint8_t *key);

int nd_network_link_test(int control, int server, const char *peer,
                         const char *interface_name, const struct ndc_hello *hello,
                         const struct ndc_challenge *challenge, const uint8_t *psk);
#endif
