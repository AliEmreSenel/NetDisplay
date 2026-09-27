#ifndef NETDISPLAY_DISPLAY_STATE_H
#define NETDISPLAY_DISPLAY_STATE_H

#include <pthread.h>
#include <stdint.h>

struct nd_display_state_source {
    pthread_mutex_t mutex;
    int session_fds[32];
    unsigned session_count;
    uint32_t flags;
    uint32_t brightness;
    char output[128];
    char source_output[128];
    char brightness_device[256];
};

int nd_display_state_start(struct nd_display_state_source *s,
                           const char *output, const char *source_output,
                           const char *brightness_device);
int nd_display_state_attach(struct nd_display_state_source *s, int fd);
void nd_display_state_detach(struct nd_display_state_source *s, int fd);
int nd_display_state_send(struct nd_display_state_source *s, int fd,
                          uint16_t type, const void *payload, uint32_t len);

#endif
