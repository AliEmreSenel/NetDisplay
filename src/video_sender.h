// SPDX-License-Identifier: GPL-2.0-only
#ifndef NETDISPLAY_VIDEO_SENDER_H
#define NETDISPLAY_VIDEO_SENDER_H
int nd_video_sender_run(const char *output_name, const char *receiver_ip,
                        int video_port, int qp_value, int width, int height,
                        int refresh_hz, uint64_t wire_session,
                        int encrypted, int key_fd);
#endif
