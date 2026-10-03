// SPDX-License-Identifier: GPL-2.0-only
#ifndef NETDISPLAY_VIDEO_SENDER_H
#define NETDISPLAY_VIDEO_SENDER_H
#include <stddef.h>
#include <stdint.h>
int nd_video_choose_encoder(uint16_t codec, int width, int height, int refresh_hz,
                            int qp_value, char *name, size_t name_size);
int nd_video_sender_run(const char *output_name, const char *receiver_ip,
                        int video_port, int qp_value, int width, int height,
                        int refresh_hz, uint64_t wire_session,
                        int encrypted, int key_fd, uint16_t video_codec,
                        const char *encoder_name);
#endif
