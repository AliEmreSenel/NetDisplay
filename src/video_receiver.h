// SPDX-License-Identifier: GPL-2.0-only
#ifndef NETDISPLAY_VIDEO_RECEIVER_H
#define NETDISPLAY_VIDEO_RECEIVER_H
#include <stddef.h>
#include <stdint.h>
#include "control_proto.h"

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
uint32_t nd_video_decoder_caps(const char *vaapi_device);
int main_receiver(int video_port, int drm_fd, int connector_id, int crtc_id,
                  const char *vaapi_device, const char *interface_name,
                  int ready_fd, int width, int height, int refresh_hz,
                  int state_fd, int encrypted, int key_fd, uint64_t wire_session,
                  uint16_t video_codec);
#endif
