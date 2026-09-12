#ifndef NETDISPLAY_VIDEO_RECEIVER_H
#define NETDISPLAY_VIDEO_RECEIVER_H
#include <stddef.h>
#include <stdint.h>

struct nd_local_display {
    uint32_t connector_id;
    uint32_t crtc_id;
    char connector[64];
    uint16_t width;
    uint16_t height;
    uint16_t refresh_hz;
};

int nd_video_open_displays(const char *requested_drm, int *master_fd,
                           char *drm_path, size_t path_size,
                           struct nd_local_display *displays, int capacity);
int nd_video_receiver_run(int video_port, int drm_fd, uint32_t connector_id,
                          uint32_t crtc_id, const char *vaapi_device,
                          const char *interface_name, int ready_fd, int width,
                          int height, int refresh_hz, int state_fd,
                          int encrypted, int key_fd);
#endif
