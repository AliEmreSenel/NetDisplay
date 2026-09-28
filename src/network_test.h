// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_NETWORK_TEST_H
#define ND_NETWORK_TEST_H
#include <stdint.h>
/* Runs before video workers start, on the negotiated video destination port.
 * Returns -1 on setup/control failure; UDP loss is a reported test result. */
int nd_network_test(int control, int server, const char *peer, int video_port,
                     const char *interface_name, int fps, uint64_t session,
                     const uint8_t *key);
#endif
