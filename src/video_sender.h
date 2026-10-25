// SPDX-License-Identifier: GPL-2.0-only
#ifndef NETDISPLAY_VIDEO_SENDER_H
#define NETDISPLAY_VIDEO_SENDER_H
#include <stddef.h>
#include <stdint.h>
#include "encoder_rate.h"
int nd_video_sender_run(const char *output_name, const char *receiver_ip,
                        int video_port, const struct nd_encoder_rate *rate,
                        int width, int height,
                        int refresh_hz, uint64_t wire_session,
                        int encrypted, int key_fd, uint16_t video_codec,
                        const char *encoder_name, unsigned keyframe_interval);
#endif
