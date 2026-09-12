#ifndef NETDISPLAY_VIDEO_RECEIVER_H
#define NETDISPLAY_VIDEO_RECEIVER_H
int nd_video_receiver_run(int video_port, const char *drm_device,
                          const char *vaapi_device, const char *interface_name,
                          int ready_fd, int width, int height, int refresh_hz,
                          int state_fd);
#endif
