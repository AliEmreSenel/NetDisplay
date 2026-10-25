// SPDX-License-Identifier: GPL-2.0-only
#ifndef ND_ENCODER_H
#define ND_ENCODER_H
#include <stddef.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>
#include "encoder_rate.h"

#define ND_DEFAULT_KEYFRAME_INTERVAL 3u
#define ND_MAX_KEYFRAME_INTERVAL 60u

struct nd_encoder {
  AVCodecContext *context;
  AVBufferRef *device, *frames;
  enum AVPixelFormat sw_format;
  unsigned keyframe_interval, frames_since_keyframe;
};
/* Software fallback is selected by name only; automatic negotiation considers
 * hardware encoders exclusively. */
int nd_encoder_open(struct nd_encoder *, const char *name, int width, int height,
                     int fps, unsigned keyframe_interval,
                     const struct nd_encoder_rate *rate,
                     enum AVPixelFormat preferred, int quiet);
void nd_encoder_close(struct nd_encoder *);
int nd_keyframe_interval_parse(const char *, unsigned *);
/* Call before transferring props to hardware frames. No B-frames/lookahead. */
void nd_encoder_prepare_frame(struct nd_encoder *, AVFrame *, int force_keyframe);
int nd_video_choose_encoder(uint16_t codec, int width, int height, int fps,
                            unsigned keyframe_interval,
                            const struct nd_encoder_rate *rate,
                            char *name, size_t name_size);
#endif
