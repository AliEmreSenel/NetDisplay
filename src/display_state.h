// SPDX-License-Identifier: GPL-2.0-only
#ifndef NETDISPLAY_DISPLAY_STATE_H
#define NETDISPLAY_DISPLAY_STATE_H

#include <pthread.h>
#include <stdint.h>

struct nd_channel;

struct nd_display_state_source {
    pthread_mutex_t mutex;
    struct nd_channel *sessions[32];
    unsigned session_count;
    uint32_t flags;
    uint32_t brightness;
    char output[128];
    char source_output[128];
    char brightness_device[256];
    void *wayland;
    void (*compositor_lost)(void *opaque);
    void *opaque;
};

/* With WAYLAND_DISPLAY or WAYLAND_SOCKET set the compositor is required and
 * start fails when it is unreachable; without either, a missing compositor
 * only disables DPMS sync and leftover-output cleanup. Once connected,
 * compositor_lost runs if the connection closes, since every output and
 * hook then belongs to a compositor that no longer exists.
 *
 * Before returning, stale_output runs for each existing output named
 * "<output>-...", which can only be left over from an earlier server. */
int nd_display_state_start(struct nd_display_state_source *s,
                           const char *output, const char *source_output,
                           const char *brightness_device,
                           void (*stale_output)(const char *name, void *opaque),
                           void (*compositor_lost)(void *opaque), void *opaque);
int nd_display_state_attach(struct nd_display_state_source *s,
                            struct nd_channel *ch);
void nd_display_state_detach(struct nd_display_state_source *s,
                             struct nd_channel *ch);
/* Sends on an attached channel, serialized with state broadcasts. */
int nd_display_state_send(struct nd_display_state_source *s,
                          struct nd_channel *ch, uint16_t type,
                          const void *payload, uint32_t len);

#endif
